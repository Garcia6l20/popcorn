#pragma once

#include <popcorn/core/types.hpp>
#include <popcorn/export.hpp>

#include <reflex/enum.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace popcorn::periph
{
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
   * @brief A STOP ended the transfer the target took part in.
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

protected:
  /** @brief Destroyed through its model only. */
  virtual ~i2c_controller_port();
};
}
