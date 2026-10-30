#pragma once

#include "i2c_link_port.hpp"

#include <popcorn/core/member_link.hpp>
#include <popcorn/export.hpp>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <vector>

namespace popcorn::periph::stm32
{
/** @brief One phase of an I2C medium as its tap shows it, stamps in system time. */
struct i2c_tap_phase
{
  /** @brief Index of the target among the medium's targets. */
  std::size_t target{};
  /** @brief Unit number the controller-side proxy gave the phase. */
  std::uint64_t unit{};
  /** @brief The phase. */
  i2c_phase phase{};
  /** @brief The address byte, 7-bit address and direction bit, for an address phase, the byte the
   * controller wrote for a write phase, the byte the target sent for a read phase. */
  std::uint8_t byte{};
  /** @brief The target's acknowledge of an address or written byte, the controller's acknowledge
   * of a read byte. */
  bool ack{};
  /** @brief End of the controller's phase, the post's stamp t. */
  core::sys_time at{};
  /** @brief Stamp of the target's answer, t + L + p + s, or the time it was dropped. */
  core::sys_time answer{};
  /** @brief Whether the target never answered the phase and gave the undriven answer. */
  bool dropped{};

  /** @brief Member-wise equality. */
  friend auto operator==(i2c_tap_phase const&, i2c_tap_phase const&) -> bool = default;
};

/**
 * @brief Phases of an I2C medium for observers, any thread. Holds at most a capacity of phases,
 *        the oldest dropped and counted. A phase enters when the controller's drain takes its
 *        answer, in drain order.
 */
class POPCORN_API i2c_tap_queue
{
public:
  /** @param capacity Phases kept, above 0. */
  explicit i2c_tap_queue(std::size_t capacity) pre(capacity > 0);

  /** @brief Appends @p phase, dropping the oldest when full. */
  void push(i2c_tap_phase const& phase);

  /** @brief Takes every phase held, oldest first. */
  auto take() -> std::vector<i2c_tap_phase>;

  /** @brief Phases dropped so far because the queue was full. */
  [[nodiscard]] auto dropped() const -> std::uint64_t;

private:
  mutable std::mutex        mutex_;
  std::deque<i2c_tap_phase> phases_;
  std::size_t               capacity_;
  std::uint64_t             dropped_{};
};

/** @brief A target of an I2C medium: its member and its I2C. */
struct i2c_target_end
{
  /** @brief Member index of the target. */
  std::size_t member{};
  /** @brief The target's I2C, which must outlive the medium's drains. */
  i2c_link_port* port{};
  /** @brief Delivery lane of the controller's posts on the target, 1 and up. */
  std::uint32_t forth_lane{};
  /** @brief Delivery lane of the target's answers on the controller, 1 and up, one per target. */
  std::uint32_t back_lane{};
};

namespace detail
{
struct i2c_pair;
}

/**
 * @brief An I2C bus across members of a linked system: one controller I2C, one target I2C per
 *        target on other members. Each target gets two links, controller to target (M->S) and
 *        target to controller (S->M), each of latency L.
 *
 *        The controller side is a link target per remote target, attached after the controller's
 *        board targets, in target order, so the controller's I2C model combines it with them as
 *        the open-drain bus does. It takes every address, since the remote match is decided only
 *        at the far side. Each address, write and read phase is posted with the stamp t of its end
 *        on the controller, the phase time p it ran with and a unit number, and the phase is left
 *        pending. The target replays it over [t + L, t + L + p], its answer is stamped
 *        t + L + p + s, s the target's stretch, and completes the controller's phase at
 *        t + 2L + p + s, on the target's S->M lane, so SCL stays low 2L + p + s per phase, a
 *        board target of the same instance included. At equal stamps the controller's own events
 *        complete first, then the answers by lane. A data phase reaches the far side only when the
 *        target acknowledged the last address. The link target answers the others at once with
 *        NACK or 0xFF and no stretch, and posts nothing. The STOP of a transfer is posted at its
 *        stamp and reaches the target at its stamp plus L. An RCC reset or a PE clear of the
 *        controller's I2C inside a transfer posts a bus release at its stamp, which ends the
 *        target's SCL low count at the stamp plus L and leaves the transfer open.
 *
 *        Each link hands a post at a drain once its far-side start, stamp plus L, lies before the
 *        drain's next barrier, and carries it to a later drain otherwise, so a port holds no entry
 *        due more than one window ahead. A post handed at a drain is late by up to one sender
 *        instruction period plus the receiver's own stop past the barrier, and keeps its own
 *        instruction count on the receiver.
 *
 *        Every phase posted gets exactly one answer. A phase the target never answers, dropped by
 *        a reset or cut by a STOP, gets NACK for an address or written byte and 0xFF for a read
 *        byte. A target reset at t_r drops the phases of stamp below t_r still in the M->S link,
 *        answered at the later of t_r and their far-side end, and discards the STOPs of stamp
 *        below t_r. A controller member reset at t_m posts a STOP at t_m when a transfer was open,
 *        forgets the controller's pending phases and mutes the controller side until the next
 *        window. Answers of stamp below t_m still in the S->M link are dropped, later answers find
 *        no phase and are counted as refused.
 *
 *        The links save the proxies' state in their own bytes, each starting with a kind tag:
 *        `i2c_forth` for M->S, `i2c_back` for S->M. A link refuses another kind's bytes with
 *        config_error.
 */
class POPCORN_API i2c_medium
{
public:
  /**
   * @param controller Member index of the controller.
   * @param controller_port The controller's I2C, which must outlive the medium's drains.
   * @param latency L, above 0.
   * @param targets The targets, at least one, none on the controller's member.
   * @param tap_capacity Phases the tap keeps.
   * @throws std::invalid_argument When @p targets is empty, a target is on the controller's
   *         member or has no port, or a lane is 0 or an answer lane is shared by two targets.
   */
  i2c_medium(
      std::size_t                 controller,
      i2c_link_port&              controller_port,
      core::sys_time              latency,
      std::vector<i2c_target_end> targets,
      std::size_t                 tap_capacity = std::size_t{1} << 20) pre(latency.ps > 0);
  /** @brief Not copyable. */
  i2c_medium(i2c_medium const&) = delete;
  /** @brief Not copy-assignable. */
  auto operator=(i2c_medium const&) -> i2c_medium& = delete;
  /** @brief Not movable. */
  i2c_medium(i2c_medium&&) = delete;
  /** @brief Not move-assignable. */
  auto operator=(i2c_medium&&) -> i2c_medium& = delete;
  /** @brief Destroys the medium. */
  ~i2c_medium();

  /**
   * @brief The links, two per target in target order, M->S then S->M. Each link and each proxy
   *        attached to an I2C keeps the state of its target alive, so they outlive the medium.
   */
  [[nodiscard]] auto links() const -> std::vector<std::shared_ptr<core::member_link>>;

  /**
   * @brief Attaches the controller-side proxies to the controller's I2C in target order. Called
   *        with the controller's host access, before the system starts. A held phase restored from
   *        a snapshot names its link target by attachment index, so every start attaches them in
   *        the same order.
   * @throws config_error As i2c_link_port::attach_target(). Nothing stays attached then.
   */
  void attach_controller();

  /**
   * @brief Attaches the proxy of target @p index to its I2C. Called with that target's host
   *        access, before the system starts.
   * @throws config_error As i2c_link_port::attach_controller().
   */
  void attach_target(std::size_t index);

  /**
   * @brief Completes every phase the controller still waits for with NACK or 0xFF at the
   *        controller's current time, then detaches the controller-side proxies. Called with the
   *        controller's host access. Does nothing for a proxy not attached.
   */
  void give_back_controller() noexcept;

  /**
   * @brief Detaches the proxy of target @p index, then answers every phase the target owes with
   *        NACK or 0xFF, stamped at its far-side end, or at the later of that end and the target
   *        reset that dropped it. Called with that target's host access. A phase the link hands to
   *        that target afterwards gets the same answer. STOPs handed afterwards are discarded.
   */
  void give_back_target(std::size_t index) noexcept pre(index < target_count());

  /** @brief The tap, any thread. */
  [[nodiscard]] auto tap() noexcept -> i2c_tap_queue&;

  /** @brief Number of targets. */
  [[nodiscard]] auto target_count() const noexcept -> std::size_t;

  /**
   * @brief Answers the controller side refused so far: no pending phase for their unit. Read with
   *        the system stopped.
   */
  [[nodiscard]] auto refused() const -> std::uint64_t;

  /** @brief Units target @p index owes an answer for. Read with the system stopped. */
  [[nodiscard]] auto owed(std::size_t index) const -> std::size_t;

  /** @brief Most units target @p index owed at once since the medium was built. */
  [[nodiscard]] auto owed_peak(std::size_t index) const -> std::size_t;

  /** @brief Phases the controller waits for on target @p index. Read with the system stopped. */
  [[nodiscard]] auto tickets(std::size_t index) const -> std::size_t;

  /** @brief Most phases the controller waited for on target @p index at once. */
  [[nodiscard]] auto tickets_peak(std::size_t index) const -> std::size_t;

private:
  i2c_link_port&                                 controller_port_;
  std::shared_ptr<i2c_tap_queue>                 tap_;
  std::vector<std::shared_ptr<detail::i2c_pair>> pairs_;
};
}
