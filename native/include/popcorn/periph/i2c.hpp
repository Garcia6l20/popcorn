#pragma once

#include <popcorn/core/types.hpp>
#include <popcorn/export.hpp>

#include <reflex/enum.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace popcorn::periph
{
class i2c_target_port;

/**
 * @brief A bus error a target injects for the byte it answers. The I2C model reports it at the
 *        end of that byte, through the flag its controller has for that condition.
 */
enum class[[= reflex::derive{reflex::Format}]] i2c_error : std::uint8_t
{
  none,
  bus_error,
  arbitration_lost,
  overrun,
};

/** @brief A target's answer to an address or to a byte the controller wrote. */
struct i2c_reply
{
  /** @brief Whether the target acknowledges. Ignored with `i2c_error::arbitration_lost`. */
  bool ack{true};
  /** @brief The error the byte ends with, none by default. */
  i2c_error error{};
  /** @brief Picoseconds the target holds SCL low after the byte, delaying the next phase. Ignored
   * with `i2c_error::arbitration_lost`. */
  std::uint64_t stretch_ps{};
};

/** @brief A target's answer to a byte the controller reads. */
struct i2c_read_reply
{
  /** @brief The byte the target sends. Dropped with `i2c_error::arbitration_lost`. */
  std::uint8_t byte{0xFF};
  /** @brief The error the byte ends with, none by default. */
  i2c_error error{};
  /** @brief Picoseconds the target holds SCL low after the byte, delaying the next phase. Ignored
   * with `i2c_error::arbitration_lost`. */
  std::uint64_t stretch_ps{};
};

/**
 * @brief A target on an I2C bus, addressed by its 7-bit address, driven by the I2C model of the
 *        controller it is attached to. Each call happens at the end of its bus phase. Times are
 *        the machine's virtual time since power-on, which a system reset does not restart.
 *
 *        Every attached target whose matches() is true takes part in a transfer, and the I2C
 *        model combines their answers as the open-drain bus does: the address or a written byte
 *        is acknowledged when one target acknowledges it, a read byte is the bitwise AND of the
 *        bytes the targets that acknowledged the address send, the stretch is the longest, and
 *        the error is the one of the first target in attachment order whose answer carries one.
 *        The model calls matches(), start_deferred(), write_deferred() and read_deferred(), whose
 *        defaults call address(), start_address(), write() and read_acked(), and those call
 *        start() and read(). A phase one target leaves pending ends at the latest completion of
 *        every answering target, with their answers combined as above in attachment order.
 */
class POPCORN_API i2c_target
{
public:
  /** @brief Creates the target. */
  i2c_target() = default;
  /** @brief Not copyable. */
  i2c_target(i2c_target const&) = delete;
  /** @brief Not copy-assignable. */
  auto operator=(i2c_target const&) -> i2c_target& = delete;
  /** @brief Not movable. */
  i2c_target(i2c_target&&) = delete;
  /** @brief Not move-assignable. */
  auto operator=(i2c_target&&) -> i2c_target& = delete;
  /** @brief Destroys the target. */
  virtual ~i2c_target();

  /** @brief The 7-bit address the target answers, 0x08 to 0x77. */
  [[nodiscard]] virtual auto address() const -> std::uint8_t = 0;

  /**
   * @brief A START or repeated START addressed the target.
   * @param read Whether the controller reads from the target.
   * @param at Virtual time since power-on at the end of the address byte.
   * @return Whether the target acknowledges its address, and the error the address byte ends
   *         with.
   */
  [[nodiscard]] virtual auto start(bool read, vtime at) -> i2c_reply = 0;

  /**
   * @brief The controller wrote one byte to the target.
   * @param byte The byte.
   * @param at Virtual time since power-on at the end of the byte.
   * @return Whether the target acknowledges the byte, and the error the byte ends with.
   */
  [[nodiscard]] virtual auto write(std::uint8_t byte, vtime at) -> i2c_reply = 0;

  /**
   * @brief The controller reads one byte from the target.
   * @param at Virtual time since power-on at the end of the byte.
   * @return The byte the target sends, and the error the byte ends with.
   */
  [[nodiscard]] virtual auto read(vtime at) -> i2c_read_reply = 0;

  /**
   * @brief The address phase observer: a 7-bit address phase to @p address ended on the bus, and
   *        the result says whether the target takes part in it. The I2C model calls it exactly
   *        once per address phase for each attached target, in attachment order, so a target may
   *        use it to observe every address phase, those to other addresses included. The default
   *        compares @p address with address().
   * @param address The 7-bit address the controller sent.
   * @param at Virtual time since power-on at the end of the address byte.
   * @return Whether the target answers the address phase.
   */
  [[nodiscard]] virtual auto matches(std::uint8_t address, [[maybe_unused]] vtime at) -> bool
  {
    return this->address() == address;
  }

  /**
   * @brief A START or repeated START addressed the target, which matches() accepted. The default
   *        calls start().
   * @param address The 7-bit address the controller sent.
   * @param read Whether the controller reads from the target.
   * @param at Virtual time since power-on at the end of the address byte.
   * @return Whether the target acknowledges the address, and the error the address byte ends
   *         with.
   */
  [[nodiscard]] virtual auto
      start_address([[maybe_unused]] std::uint8_t address, bool read, vtime at) -> i2c_reply
  {
    return start(read, at);
  }

  /**
   * @brief The controller reads one byte from the target and acknowledges it or not at its
   *        ninth clock. The default calls read().
   * @param at Virtual time since power-on at the end of the byte.
   * @param ack Whether the controller acknowledges the byte: false for the last byte of its
   *        transfer.
   * @return The byte the target sends, and the error the byte ends with.
   */
  [[nodiscard]] virtual auto read_acked(vtime at, [[maybe_unused]] bool ack) -> i2c_read_reply
  {
    return read(at);
  }

  /**
   * @brief A START or repeated START addressed the target, which matches() accepted, with an
   *        answer the target may give later. The I2C model calls only this form for an address
   *        phase. An answer given here completes the phase at once, as start_address() does. An
   *        empty result leaves the phase pending: the target completes it through
   *        i2c_target_port::complete() with @p ticket, and SCL stays low until then. The default
   *        returns start_address().
   * @param address The 7-bit address the controller sent.
   * @param read Whether the controller reads from the target.
   * @param at Virtual time since power-on at the end of the address byte.
   * @param ticket Identifies this target's answer to this phase to i2c_target_port::complete().
   * @return The answer, as for start_address(), or empty while it is pending.
   */
  [[nodiscard]] virtual auto
      start_deferred(std::uint8_t address, bool read, vtime at, std::uint64_t ticket)
          -> std::optional<i2c_reply>
  {
    static_cast<void>(ticket);
    return start_address(address, read, at);
  }

  /**
   * @brief The controller wrote one byte to the target, with an answer the target may give
   *        later. The I2C model calls only this form for a written byte. An empty result leaves
   *        the phase pending until i2c_target_port::complete() with @p ticket. The default returns
   *        write().
   * @param byte The byte.
   * @param at Virtual time since power-on at the end of the byte.
   * @param ticket Identifies this target's answer to this phase to i2c_target_port::complete().
   * @return The answer, as for write(), or empty while it is pending.
   */
  [[nodiscard]] virtual auto write_deferred(std::uint8_t byte, vtime at, std::uint64_t ticket)
      -> std::optional<i2c_reply>
  {
    static_cast<void>(ticket);
    return write(byte, at);
  }

  /**
   * @brief The controller reads one byte from the target, with an answer the target may give
   *        later. The I2C model calls only this form for a read byte. An empty result leaves the
   *        phase pending until i2c_target_port::complete_read() with @p ticket. The default
   *        returns read_acked().
   * @param at Virtual time since power-on at the end of the byte.
   * @param ack Whether the controller acknowledges the byte, as for read_acked().
   * @param ticket Identifies this target's answer to this phase to
   *        i2c_target_port::complete_read().
   * @return The byte and error, as for read_acked(), or empty while they are pending.
   */
  [[nodiscard]] virtual auto read_deferred(vtime at, bool ack, std::uint64_t ticket)
      -> std::optional<i2c_read_reply>
  {
    static_cast<void>(ticket);
    return read_acked(at, ack);
  }

  /**
   * @brief The target was attached to the I2C model of an instance, at the model's construction
   *        for a board target. The port stays valid until detached(). The default does nothing.
   * @param port The model the target completes pending phases through.
   */
  virtual void attached([[maybe_unused]] i2c_target_port& port)
  {}

  /**
   * @brief The target was detached from the port attached() gave, when the model is destroyed
   *        for a board target. The target keeps no reference to that port after the call. The
   *        default does nothing.
   */
  virtual void detached() noexcept
  {}

  /**
   * @brief A STOP ended the transfer the target took part in. A transfer runs from its START to
   *        its STOP across every repeated START, so a target an earlier address of the transfer
   *        matched hears the STOP too.
   * @param at Virtual time since power-on of the STOP condition.
   */
  virtual void stop(vtime at) = 0;

  /**
   * @brief The machine was reset. The I2C model calls it for every attached target, in
   *        attachment order, after its own reset. The default does nothing.
   * @param kind Cause of the reset. A target on the board takes power_on and brown_out as its
   *        own supply events and any other kind as an MCU reset it does not see. After power_on
   *        the times passed to later calls restart from 0, so on power_on and brown_out a device
   *        clears every time it keeps along with its volatile state.
   */
  virtual void reset([[maybe_unused]] reset_kind kind)
  {}

  /**
   * @brief The supply was cut. The I2C model calls it for every attached target, in
   *        attachment order, from its own power_cut(), before the power-on reset of
   *        `core::machine::power_cycle()`. The default does nothing. A target holding a
   *        non-volatile array applies what the cut leaves of an operation in flight. When a
   *        target throws, every later target is still cut, and the first exception is rethrown
   *        after.
   * @param at Virtual time since power-on of the cut.
   */
  virtual void power_cut([[maybe_unused]] vtime at)
  {}

  /**
   * @brief Appends the target state to a snapshot.
   * @param out Snapshot bytes, appended to.
   */
  virtual void save(std::vector<std::byte>& out) const = 0;

  /**
   * @brief Restores the target state from bytes save() wrote, so that every later call behaves
   *        as it would have after the save.
   * @param in Bytes of one save() call.
   */
  virtual void load(std::span<std::byte const> in) = 0;
};

/**
 * @brief What the I2C model of an instance offers the i2c_target targets attached to it, to
 *        complete the phases they left pending in i2c_target::start_deferred(),
 *        i2c_target::write_deferred() and i2c_target::read_deferred(). A call only schedules
 *        the completion, one event on its lane, which fires after the model's own events due at
 *        the same instant, in the order of the lanes. When it falls due, a completion whose
 *        ticket is not a pending answer of the phase still held, after a reset, after PE was
 *        cleared, for an answer already given or of another kind of phase, is refused and
 *        counted in the model's saved state, never applied. The target releases SCL at the
 *        completion's time plus the answer's stretch_ps.
 */
class POPCORN_API i2c_target_port
{
public:
  /**
   * @brief Completes a pending address or write phase at @p at with @p reply.
   * @param ticket The ticket start_deferred() or write_deferred() received.
   * @param reply The target's acknowledge, error and stretch.
   * @param at Virtual time since power-on of the completion. A due time in the past keeps its
   *        own instruction count, as a post a link hands at a drain may have.
   * @param lane Delivery lane of the completion, 1 and up.
   * @throws std::invalid_argument When @p at is before the last reset, or @p lane is 0.
   */
  virtual void complete(std::uint64_t ticket, i2c_reply reply, vtime at, std::uint32_t lane) = 0;

  /**
   * @brief Completes a pending read phase at @p at with @p reply.
   * @param ticket The ticket read_deferred() received.
   * @param reply The byte the target sends, its error and stretch.
   * @param at Virtual time since power-on of the completion, as for complete().
   * @param lane Delivery lane of the completion, 1 and up.
   * @throws std::invalid_argument As complete().
   */
  virtual void
      complete_read(std::uint64_t ticket, i2c_read_reply reply, vtime at, std::uint32_t lane) = 0;

protected:
  /** @brief Destroyed through its model only. */
  virtual ~i2c_target_port();
};

/** @brief The bus phase of a unit an i2c_controller_port queues. */
enum class[[= reflex::derive{reflex::Format}]] i2c_phase : std::uint8_t
{
  address,
  write,
  read,
};

/** @brief What the I2C model of an instance answers its controller for one phase. */
struct i2c_answer
{
  /** @brief Whether the MCU acknowledged the address or the written byte. A read phase carries
   * the acknowledge its controller queued. */
  bool ack{};
  /** @brief The byte the MCU sent in a read phase, 0xFF for the other phases. */
  std::uint8_t byte{0xFF};
  /** @brief The error the phase ends with, none by default. */
  i2c_error error{};
};

/**
 * @brief A controller on an I2C bus, outside the MCU, addressing an I2C in target mode. It
 *        queues phases through the instance's i2c_controller_port and hears each phase back
 *        here, on the machine thread. One per I2C instance, none next to i2c_target targets.
 *        Times are the machine's time since its last reset.
 */
class POPCORN_API i2c_controller_sink
{
public:
  /** @brief Creates the controller. */
  i2c_controller_sink() = default;
  /** @brief Not copyable. */
  i2c_controller_sink(i2c_controller_sink const&) = delete;
  /** @brief Not copy-assignable. */
  auto operator=(i2c_controller_sink const&) -> i2c_controller_sink& = delete;
  /** @brief Not movable. */
  i2c_controller_sink(i2c_controller_sink&&) = delete;
  /** @brief Not move-assignable. */
  auto operator=(i2c_controller_sink&&) -> i2c_controller_sink& = delete;
  /** @brief Destroys the controller. */
  virtual ~i2c_controller_sink();

  /**
   * @brief One phase ended and the MCU answered it.
   * @param unit The unit the phase was queued with.
   * @param phase The phase.
   * @param answer The MCU's acknowledge or byte.
   * @param at Machine time since reset at which SCL is released after the phase: its end, or
   *        the end of the MCU's clock stretch after it.
   */
  virtual void answered(std::uint64_t unit, i2c_phase phase, i2c_answer answer, vtime at) = 0;

  /**
   * @brief A phase or unit the I2C model will never answer, reported once, on the machine
   *        thread, from the event of the lane the unit was queued on. A reset of the target, a
   *        STOP due before the phase's end or i2c_controller_port::drop() drops a unit. Its
   *        undriven answer is NACK for an address or a written byte, 0xFF for a read byte. The
   *        default does nothing.
   * @param unit The unit the phase was queued with.
   * @param phase The phase.
   * @param at Machine time since reset at which the unit is reported: the phase's end, or the
   *        reset's time when that is later, or the STOP's time, or the time drop() gave.
   */
  virtual void dropped(
      [[maybe_unused]] std::uint64_t unit,
      [[maybe_unused]] i2c_phase     phase,
      [[maybe_unused]] vtime         at)
  {}
};

/**
 * @brief What the I2C model of an instance offers the i2c_controller_sink attached to it. Each
 *        primitive carries a @p unit strictly above the one before. The port runs the entries
 *        due at one instruction count in unit order, after the events of lower lanes, whatever
 *        order they were queued in. A due time in the past keeps its own instruction count. SCL
 *        stays low between phases until the next one starts.
 */
class POPCORN_API i2c_controller_port
{
public:
  /** @brief Most entries queued at once. */
  static constexpr std::size_t max_entries = 4096;

  /**
   * @brief Queues a START, or a repeated START when no stop() came before it, and the address
   *        byte, on @p lane, from the latest of @p at, the end of every phase queued before it
   *        and the time of every queued stop(). A phase whose start is at or before the last RCC
   *        reset of the instance is dropped, reported at the later of its end and that reset. A
   *        phase a reset drops still holds the wire until its end, rebased by a machine reset.
   * @param address 7-bit address, 0 to 0x7F.
   * @param read Whether the controller reads from the MCU.
   * @param phase_ps Length of the phase, the START, eight address bits and the acknowledge, at
   *        least 1.
   * @param at Machine time since reset of the earliest start.
   * @param lane Delivery lane, 1 and up.
   * @param unit Unit number.
   * @throws config_error When no controller is attached to the instance, or the port would hold
   *         more than max_entries entries.
   * @throws std::invalid_argument When @p lane is 0, @p unit is not above the last unit, an
   *         argument is out of its range or the phase ends past the time range.
   */
  virtual void start(
      std::uint8_t  address,
      bool          read,
      std::uint64_t phase_ps,
      vtime         at,
      std::uint32_t lane,
      std::uint64_t unit) = 0;

  /**
   * @brief Queues one byte the controller writes, as start() places it.
   * @param byte The byte.
   * @param phase_ps Length of the phase, eight bits and the acknowledge, at least 1.
   * @param at Machine time since reset of the earliest start.
   * @param lane Delivery lane, 1 and up.
   * @param unit Unit number.
   * @throws config_error As start().
   * @throws std::invalid_argument As start().
   */
  virtual void write(
      std::uint8_t  byte,
      std::uint64_t phase_ps,
      vtime         at,
      std::uint32_t lane,
      std::uint64_t unit) = 0;

  /**
   * @brief Queues one byte the controller reads, as start() places it.
   * @param ack The acknowledge the controller gives the byte.
   * @param phase_ps Length of the phase, eight bits and the acknowledge, at least 1.
   * @param at Machine time since reset of the earliest start.
   * @param lane Delivery lane, 1 and up.
   * @param unit Unit number.
   * @throws config_error As start().
   * @throws std::invalid_argument As start().
   */
  virtual void
      read(bool ack, std::uint64_t phase_ps, vtime at, std::uint32_t lane, std::uint64_t unit) = 0;

  /**
   * @brief Queues a STOP at the later of @p at and the last RCC reset of the instance, on
   *        @p lane. Every phase of a lower unit still queued or running then is dropped and
   *        reported at that time. A unit already dropped keeps its own report time. A member or
   *        machine reset of the target removes every queued STOP.
   * @param at Machine time since reset.
   * @param lane Delivery lane, 1 and up.
   * @param unit Unit number.
   * @throws config_error As start().
   * @throws std::invalid_argument When @p lane is 0 or @p unit is not above the last unit.
   */
  virtual void stop(vtime at, std::uint32_t lane, std::uint64_t unit) = 0;

  /**
   * @brief Queues a unit the caller knows is lost, reported through
   *        i2c_controller_sink::dropped() at the later of @p at and the last RCC reset of the
   *        instance.
   * @param phase The phase the unit was.
   * @param at Machine time since reset.
   * @param lane Delivery lane, 1 and up.
   * @param unit Unit number.
   * @throws config_error As start().
   * @throws std::invalid_argument As stop(), or when @p phase is out of its range.
   */
  virtual void drop(i2c_phase phase, vtime at, std::uint32_t lane, std::uint64_t unit) = 0;

  /**
   * @brief The unit of the last entry queued, kept across resets and snapshots, so the next
   *        primitive takes a unit above it. The default returns 0, for a port that queues no
   *        entry: a port that queues entries overrides last_unit() and queued().
   */
  [[nodiscard]] virtual auto last_unit() const -> std::uint64_t
  {
    return 0;
  }

  /**
   * @brief The entries queued now, at most max_entries. The default returns 0, for a port that
   *        queues no entry: a port that queues entries overrides last_unit() and queued().
   */
  [[nodiscard]] virtual auto queued() const -> std::size_t
  {
    return 0;
  }

protected:
  /** @brief Destroyed through its model only. */
  virtual ~i2c_controller_port();
};
}
