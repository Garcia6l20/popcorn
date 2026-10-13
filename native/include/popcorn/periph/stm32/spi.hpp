#pragma once

#include <popcorn/core/types.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace popcorn::periph::stm32
{
/**
 * @brief A device on an SPI bus, driven by the SPI model of the instance it is attached to. Full
 *        duplex: every frame out returns a frame in. Times are the machine's virtual time since
 *        power-on, which a system reset does not restart.
 */
class spi_target
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
  virtual ~spi_target() = default;

  /**
   * @brief The chip select changed.
   * @param selected Whether the device is now selected, chip select low.
   * @param at Virtual time since power-on of the edge.
   */
  virtual void select(bool selected, vtime at) = 0;

  /**
   * @brief Exchanges one frame while the device is selected.
   * @param out Frame the controller sends, right-aligned.
   * @param bits Frame size, 4 to 32.
   * @param at Virtual time since power-on at the end of the frame.
   * @return Frame the device returns, right-aligned, bits above @p bits ignored.
   */
  [[nodiscard]] virtual auto exchange(std::uint32_t out, std::uint8_t bits, vtime at)
      -> std::uint32_t = 0;

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
   * @brief Appends the device state to a snapshot.
   * @param out Snapshot bytes, appended to.
   */
  virtual void save(std::vector<std::byte>& out) const = 0;

  /**
   * @brief Restores the device state from bytes save() wrote.
   * @param in Bytes of one save() call.
   */
  virtual void load(std::span<std::byte const> in) = 0;
};

/**
 * @brief One device attached to an SPI instance, and the GPIO pin selecting it, active low.
 */
struct spi_attachment
{
  /** @brief The device. */
  std::shared_ptr<spi_target> device;
  /** @brief GPIO port of the chip select, such as `GPIOA`. */
  std::string cs_port{};
  /** @brief Chip select pin, 0 to 15. Empty when the SPI hardware SS output selects the device:
   * selected from CSTART to the end of the transfer while CFG2.SSOE is set. */
  std::optional<std::uint8_t> cs_pin{};
};

/**
 * @brief A controller on an SPI bus, outside the MCU, clocking the SPI in target mode. It
 *        drives frames through the instance's spi_controller_port and hears each frame back
 *        here. One per SPI instance, none next to spi_target devices.
 */
class spi_controller_sink
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
  virtual ~spi_controller_sink() = default;

  /**
   * @brief One frame of a transfer ended.
   * @param sent Frame the controller sent, right-aligned.
   * @param received Frame the controller received from the MCU, all ones when the SPI did not
   *        drive MISO, right-aligned.
   * @param at Machine time since reset at the end of the frame.
   */
  virtual void frame(std::uint32_t sent, std::uint32_t received, vtime at) = 0;
};

/**
 * @brief What the SPI model of an instance offers the spi_controller_sink attached to it.
 */
class spi_controller_port
{
public:
  /** @brief Most frames queued at once over every transfer not finished. */
  static constexpr std::size_t max_frames = 4096;

  /**
   * @brief Queues one transfer: the NSS input selected at its start, @p frames shifted one
   *        frame time apart, NSS released at the end of the last one. A transfer starts at the
   *        later of @p start and the end of the transfers queued before it.
   * @param start Machine time since reset of the earliest start, not before the current time.
   * @param frame_ps Length of one frame, at least 1.
   * @param bits Frame size, 4 to 32.
   * @param frames The frames, 1 to max_frames, each below 2 to the @p bits.
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
  ~spi_controller_port() = default;
};
}
