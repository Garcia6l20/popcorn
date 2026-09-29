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
}
