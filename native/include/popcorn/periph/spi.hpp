#pragma once

#include <popcorn/core/types.hpp>
#include <popcorn/export.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace popcorn::periph
{
/**
 * @brief A device on an SPI bus, driven by the SPI model of the controller it is attached to.
 *        Full duplex: every frame out returns a frame in. Frames cross in wire order,
 *        right-aligned, the first bit on the wire in bit `bits - 1`. Times are the machine's
 *        virtual time since power-on, which a system reset does not restart.
 */
class POPCORN_API spi_target
{
public:
  /** @brief Creates the device. */
  spi_target() = default;
  /** @brief Not copyable. */
  spi_target(spi_target const&) = delete;
  /** @brief Not copy-assignable. */
  auto operator=(spi_target const&) -> spi_target& = delete;
  /** @brief Not movable. */
  spi_target(spi_target&&) = delete;
  /** @brief Not move-assignable. */
  auto operator=(spi_target&&) -> spi_target& = delete;
  /** @brief Destroys the device. */
  virtual ~spi_target();

  /**
   * @brief The chip select changed.
   * @param selected Whether the device is now selected, chip select low.
   * @param at Virtual time since power-on of the edge.
   */
  virtual void select(bool selected, vtime at) = 0;

  /**
   * @brief Exchanges one frame while the device is selected.
   * @param out Frame the controller sends, in wire order, right-aligned.
   * @param bits Frame size, 1 to 32, the size the firmware configured.
   * @param at Virtual time since power-on at the end of the frame.
   * @return Frame the device returns, in wire order, right-aligned, bits above @p bits ignored.
   */
  [[nodiscard]] virtual auto exchange(std::uint32_t out, std::uint8_t bits, vtime at)
      -> std::uint32_t = 0;

  /**
   * @brief The SPI mode the device works in, CPOL << 1 | CPHA. The SPI model reads it once per
   *        transfer and device, at the first frame exchanged, and fails the run with
   *        config_error when the firmware clocks the bus in another mode. The default declares
   *        none, which is never checked.
   * @return 0 to 3, or empty for no check.
   */
  [[nodiscard]] virtual auto mode() const -> std::optional<std::uint8_t>
  {
    return std::nullopt;
  }

  /**
   * @brief The machine was reset. The SPI model calls it for every attached device, in
   *        attachment order, after its own reset. The default does nothing.
   * @param kind Cause of the reset. A device on the board takes power_on and brown_out as its
   *        own supply events and any other kind as an MCU reset it does not see. After power_on
   *        the times passed to later calls restart from 0, so on power_on and brown_out a device
   *        clears every time it keeps along with its volatile state.
   */
  virtual void reset([[maybe_unused]] reset_kind kind)
  {}

  /**
   * @brief The supply was cut. The SPI model calls it for every attached device, in
   *        attachment order, from its own power_cut(), before the power-on reset of
   *        `core::machine::power_cycle()`. The default does nothing. A device holding a
   *        non-volatile array applies what the cut leaves of an operation in flight. When a
   *        device throws, every later device is still cut, and the first exception is rethrown
   *        after.
   * @param at Virtual time since power-on of the cut.
   */
  virtual void power_cut([[maybe_unused]] vtime at)
  {}

  /**
   * @brief Appends the device state to a snapshot.
   * @param out Snapshot bytes, appended to.
   */
  virtual void save(std::vector<std::byte>& out) const = 0;

  /**
   * @brief Restores the device state from bytes save() wrote, so that every later call behaves
   *        as it would have after the save.
   * @param in Bytes of one save() call.
   */
  virtual void load(std::span<std::byte const> in) = 0;
};

/**
 * @brief A controller on an SPI bus, outside the MCU, clocking an SPI in target mode. It drives
 *        frames through the instance's spi_controller_port and hears each frame back here. One
 *        per SPI instance, none next to spi_target devices. Frames cross in wire order, as for
 *        spi_target. Times are the machine's time since its last reset.
 */
class POPCORN_API spi_controller_sink
{
public:
  /** @brief Creates the controller. */
  spi_controller_sink() = default;
  /** @brief Not copyable. */
  spi_controller_sink(spi_controller_sink const&) = delete;
  /** @brief Not copy-assignable. */
  auto operator=(spi_controller_sink const&) -> spi_controller_sink& = delete;
  /** @brief Not movable. */
  spi_controller_sink(spi_controller_sink&&) = delete;
  /** @brief Not move-assignable. */
  auto operator=(spi_controller_sink&&) -> spi_controller_sink& = delete;
  /** @brief Destroys the controller. */
  virtual ~spi_controller_sink();

  /**
   * @brief One frame of a transfer ended.
   * @param sent Frame the controller sent, in wire order, right-aligned.
   * @param received Frame the controller received from the MCU, in wire order, all ones when
   *        the SPI did not drive MISO, right-aligned.
   * @param at Machine time since reset at the end of the frame.
   */
  virtual void frame(std::uint32_t sent, std::uint32_t received, vtime at) = 0;

  /**
   * @brief The SPI mode the controller clocks in, CPOL << 1 | CPHA. The SPI model reads it once
   *        per transfer, at the first frame the target takes part in, and fails the run with
   *        config_error when the firmware configured another mode. The default declares none,
   *        which is never checked.
   * @return 0 to 3, or empty for no check.
   */
  [[nodiscard]] virtual auto mode() const -> std::optional<std::uint8_t>
  {
    return std::nullopt;
  }
};

/**
 * @brief What the SPI model of an instance offers the spi_controller_sink attached to it.
 */
class POPCORN_API spi_controller_port
{
public:
  /** @brief Most frames queued at once over every transfer not finished. */
  static constexpr std::size_t max_frames = 4096;

  /**
   * @brief Queues one transfer: the chip select input selected at its start, @p frames shifted
   *        one frame time apart, chip select released at the end of the last one. A transfer
   *        starts at the later of @p start and the end of the transfers queued before it.
   * @param start Machine time since reset of the earliest start, not before the current time.
   * @param frame_ps Length of one frame, at least 1.
   * @param bits Frame size, 4 to 32.
   * @param frames The frames in wire order, 1 to max_frames, each below 2 to the @p bits.
   * @return Machine time since reset at which the transfer starts.
   * @throws config_error When no controller is attached to the instance, or the queue would
   *         hold more than max_frames frames.
   * @throws std::invalid_argument When @p start is past, the transfer ends past the time
   *         range, or an argument is out of its range.
   */
  virtual auto transfer(
      vtime                          start,
      std::uint64_t                  frame_ps,
      std::uint8_t                   bits,
      std::span<std::uint32_t const> frames) -> vtime = 0;

protected:
  /** @brief Destroyed through its model only. */
  virtual ~spi_controller_port();
};
}
