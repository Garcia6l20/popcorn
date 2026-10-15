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
}
