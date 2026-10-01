#pragma once

#include <popcorn/core/fwd.hpp>
#include <popcorn/core/types.hpp>

#include <cmath>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace popcorn::core
{
/** @brief Handle of a named clock domain, such as an APB bus, resolved once by name. */
struct clock_domain
{
  /** @brief Index of the domain within its clock, stable across restores. */
  std::uint32_t index;

  /** @brief Member-wise equality. */
  friend auto operator==(clock_domain, clock_domain) -> bool = default;
};

/**
 * @brief Plain-data state of a clock, what a snapshot saves and restores.
 */
struct clock_state
{
  /** @brief Instructions executed since reset. */
  icount now{};
  /** @brief Instruction count of the last rate change. */
  icount epoch_icount{};
  /** @brief Virtual time of the last rate change. */
  vtime epoch_time{};
  /** @brief Virtual time in picoseconds run before the last reset, summed over every reset since
   * the last power-on reset. */
  std::uint64_t before_reset_ps{};
  /** @brief Core clock frequency in Hz. */
  std::uint64_t core_hz{};
  /** @brief Core cycles per instruction. */
  double cpi{1.0};
  /** @brief Clock domain names, the index is the clock_domain handle. */
  std::vector<std::string> domain_names;
  /** @brief Clock domain frequencies in Hz, 0 while unset, same indices as domain_names. */
  std::vector<std::uint64_t> domain_hz;

  /** @brief Member-wise equality. */
  friend auto operator==(clock_state const&, clock_state const&) -> bool = default;
};

/**
 * @brief Virtual clock of a machine. Time is counted in executed instructions. Virtual time is
 *        piecewise linear: each rate change starts an epoch, and time within an epoch is
 *        vtime_from_instructions() of the instructions since its start. Never reads wall time.
 *        Neither copyable nor movable: models and the clock_driver hold references to it.
 */
class clock
{
public:
  /**
   * @brief Creates a clock at instruction 0 and time 0.
   * @param core_hz Core clock frequency in Hz.
   * @param cpi Core cycles per instruction, at least one instruction per second must result.
   */
  explicit clock(std::uint64_t core_hz, double cpi = 1.0) pre(core_hz > 0) pre(cpi > 0.0)
      pre(static_cast<double>(core_hz) / cpi >= 1.0);

  /** @brief Not copyable. */
  clock(clock const&) = delete;
  /** @brief Not copy-assignable. */
  auto operator=(clock const&) -> clock& = delete;
  /** @brief Not movable. */
  clock(clock&&) = delete;
  /** @brief Not move-assignable. */
  auto operator=(clock&&) -> clock& = delete;
  /** @brief Destroys the clock. */
  ~clock();

  /**
   * @brief Instructions executed since reset, exact to the translated block: the block hook
   *        advances the clock once per block, so the clock is the instruction counter. A block
   *        the engine leaves before its end, for any reason, an intercept that returns or stops,
   *        a code hook stop, an exception or a fault, counts its full length: deterministic, not
   *        exact. Instructions single-stepped for exact_instructions count one by one.
   */
  [[nodiscard]] auto now() const noexcept -> icount
  {
    return state_.now;
  }

  /** @brief Core clock frequency in Hz. */
  [[nodiscard]] auto core_hz() const noexcept -> std::uint64_t
  {
    return state_.core_hz;
  }

  /** @brief Core cycles per instruction. */
  [[nodiscard]] auto cpi() const noexcept -> double
  {
    return state_.cpi;
  }

  /**
   * @brief Instructions executed per virtual second: core_hz() / cpi(), rounded to the nearest
   *        Hz. Equal to core_hz() at a cpi of 1.0.
   */
  [[nodiscard]] auto instruction_hz() const noexcept -> std::uint64_t
  {
    return static_cast<std::uint64_t>(
        std::llround(static_cast<double>(state_.core_hz) / state_.cpi));
  }

  /**
   * @brief Virtual time since reset: the time of the current epoch's start plus
   *        vtime_from_instructions() of the instructions executed since, at instruction_hz().
   */
  [[nodiscard]] auto time() const -> vtime
  {
    return vtime{
        state_.epoch_time.ps
        + vtime_from_instructions(state_.now.value - state_.epoch_icount.value, instruction_hz())
              .ps};
  }

  /**
   * @brief Virtual time since the last power-on reset: time() plus the time run before every
   *        other kind of reset since. What a backup-domain device such as an RTC keeps counting
   *        through a system reset.
   */
  [[nodiscard]] auto since_power_on() const -> vtime
  {
    return vtime{state_.before_reset_ps + time().ps};
  }

  /**
   * @brief Instruction count at which a virtual time is reached at the current rate, rounded up
   *        through icount_from_vtime(), so an event scheduled for @p at never fires before it.
   *        A deadline that must hold across rate changes is scheduled with
   *        event_port::schedule_at, not converted here.
   * @param at Virtual time since reset, not before the current epoch's start.
   */
  [[nodiscard]] auto to_icount(vtime at) const -> icount pre(at.ps >= state_.epoch_time.ps)
  {
    return icount{
        state_.epoch_icount.value
        + icount_from_vtime(vtime{at.ps - state_.epoch_time.ps}, instruction_hz()).value};
  }

  /**
   * @brief Changes the core clock frequency from now on, starting a new epoch at now(), exact to
   *        the block in which firmware made the change.
   *
   * Pending deadline::time events move to the instruction count of their virtual time at the
   * new rate, deadline::instructions events keep theirs.
   * @param hz Frequency in Hz, at least one instruction per second must result.
   */
  void set_core_hz(std::uint64_t hz) pre(hz > 0) pre(static_cast<double>(hz) / cpi() >= 1.0);

  /**
   * @brief Changes the cycles per instruction from now on, starting a new epoch at now().
   *        Pending deadline::time events move to the instruction count of their virtual time
   *        at the new rate, deadline::instructions events keep theirs.
   * @param cpi Core cycles per instruction, at least one instruction per second must result.
   */
  void set_cpi(double cpi) pre(cpi > 0.0) pre(static_cast<double>(core_hz()) / cpi >= 1.0);

  /**
   * @brief Resolves a clock domain by name, creating it unset when absent. Called once when a
   *        model attaches, the handle then serves the hot path and stays valid across restores.
   * @param name Domain name, such as `apb1` or `pclk2`.
   */
  [[nodiscard]] auto domain(std::string_view name) -> clock_domain;

  /**
   * @brief Current frequency of a clock domain.
   * @param domain Handle from domain().
   * @return Frequency in Hz. 0 for a stopped clock: a domain nothing has set yet, a gated
   *         peripheral clock or a source that is off. A caller that divides by it checks for 0
   *         first. The core clock is not a domain, see core_hz(), and is never 0.
   */
  [[nodiscard]] auto hz(clock_domain domain) const -> std::uint64_t
      pre(domain.index < state_.domain_hz.size())
  {
    return state_.domain_hz[domain.index];
  }

  /**
   * @brief Sets the frequency of a clock domain, as the RCC model does when firmware reprograms
   *        the clock tree.
   * @param domain Handle from domain().
   * @param hz Frequency in Hz, 0 for a stopped clock: a gated peripheral clock or a source that
   *        is off. The core clock is set through set_core_hz(), which keeps it above 0.
   */
  void set_hz(clock_domain domain, std::uint64_t hz) pre(domain.index < state_.domain_hz.size());

private:
  friend class clock_driver;

  void start_epoch();

  clock_state                    state_;
  std::function<void()>          on_epoch_;
  std::function<std::uint64_t()> charged_ahead_;
};

/**
 * @brief Moves a clock forward and saves or restores it for the run loop. Owned by the machine
 *        and never given to models, which only read time and set frequencies through clock.
 */
class clock_driver
{
public:
  /**
   * @brief Creates a driver for a clock.
   * @param driven Clock to drive, must outlive the driver.
   */
  explicit clock_driver(clock& driven) noexcept : clock_{driven}
  {}

  /**
   * @brief Adds the instructions of one translated block. Called by the block hook for every
   *        block, not once per slice, so now() is exact to the block for models and rate changes.
   * @param executed Instructions in the block, as the block hook counts them.
   */
  void advance(icount executed) noexcept
  {
    clock_.state_.now.value += executed.value;
  }

  /**
   * @brief Moves time to a deadline while the core idles, as a tickless WFI does.
   * @param deadline Instruction count to move to, not before now.
   */
  void jump_to(icount deadline) noexcept pre(deadline.value >= clock_.state_.now.value)
  {
    clock_.state_.now = deadline;
  }

  /** @brief Copy of the clock state, for a snapshot. */
  [[nodiscard]] auto state() const -> clock_state
  {
    return clock_.state_;
  }

  /**
   * @brief Restores a snapshot's clock state. Domain handles stay valid: the snapshot's domain
   *        names and the clock's must agree index by index on their common prefix. A domain the
   *        clock resolved after the snapshot keeps its index and restarts unset, at 0 Hz, as a
   *        fresh replay would have it. A domain only the snapshot has is added at its index.
   * @param saved State from state().
   */
  void restore(clock_state const& saved) pre(saved.domain_names.size() == saved.domain_hz.size());

  /**
   * @brief Installs the function the clock calls after every rate change, once the new epoch
   *        has started. The machine re-keys its time-keyed events there.
   * @param hook Called with no argument, empty to remove.
   */
  void on_rate_change(std::function<void()> hook);

  /**
   * @brief Installs the function a rate change asks for the instructions already charged to the
   *        clock but not retired yet, those after the current one in a translated block charged
   *        whole. The new epoch starts before them, so they run at the new rate.
   * @param ahead Returns the count, 0 when none or unknown, empty to remove.
   */
  void on_charged_ahead(std::function<std::uint64_t()> ahead);

private:
  clock& clock_;
};
}
