#pragma once

#include <popcorn/periph/spi.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace popcorn::periph::stm32
{
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
