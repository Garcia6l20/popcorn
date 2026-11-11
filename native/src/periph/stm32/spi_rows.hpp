#pragma once

#include "bus_window.hpp"
#include "row_of.hpp"

#include <popcorn/periph/stm32/board.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

namespace popcorn::periph::stm32
{
/** @brief Differences between the SPI peripherals the model rows serve. */
struct spi_variant
{
  /** @brief Instances with an 8-byte FIFO, the others hold 16 bytes. */
  std::span<std::string_view const> limited;
  /** @brief The limited instances have the `CTSIZE` remaining-frames counter. */
  bool limited_counter = false;
  /** @brief The peripheral has the `SPI_AUTOCR` register and its trigger field. */
  bool autocr = true;
  /** @brief `SPI_CFG1` TXDMAEN and RXDMAEN stay writable while SPE = 1, the rest of the register
   * is write-protected then. Otherwise the whole register is. */
  bool dma_enables_writable = true;
};

inline constexpr auto u5_limited = std::to_array<std::string_view>({"SPI3"});
/** @brief STM32U5 SPI, RM0456 68.3: SPI3 has an 8-byte FIFO and no `CTSIZE`. RM0456 68.8.3:
 *         `SPI_CFG1` is write-protected while SPE = 1 except TXDMAEN and RXDMAEN. */
inline constexpr spi_variant u5_spi{u5_limited};

inline constexpr auto h5_limited = std::to_array<std::string_view>({"SPI4", "SPI5", "SPI6"});
/** @brief STM32H5 SPI, RM0481 Table 645: SPI4 to SPI6 have an 8-byte FIFO and keep TSIZE up to
 *         65535, so `CTSIZE` counts there. RM0481 section 58.11 has no `SPI_AUTOCR`. RM0481
 *         58.11.3: the whole `SPI_CFG1` is write-protected while SPE = 1. */
inline constexpr spi_variant h5_spi{
    .limited              = h5_limited,
    .limited_counter      = true,
    .autocr               = false,
    .dma_enables_writable = false};

/** @brief Differences between the classic SPI peripherals the model rows serve. */
struct spi_l1l4_variant
{
  /** @brief The peripheral has the TI frame format, `CR2.FRF`. Without it the bit is storage. */
  bool ti_mode;
  /**
   * @brief The peripheral has the 32-bit transmit and receive FIFOs, frames of `CR2.DS` + 1 bits
   *        and `CR2.FRXTH`, RM0351 42.4.9. Without it, one buffer each way and `CR1.DFF`.
   */
  bool fifo = false;
  /** @brief Buses whose clock is the clock of an instance in their window. */
  std::span<bus_window const> kernel_buses;
};

/** @brief STM32L1 SPI of Cat.3 to Cat.6, RM0038 28.2.1: the TI mode. */
inline constexpr spi_l1l4_variant l1_spi_ti{.ti_mode = true, .kernel_buses = l1_apb_windows};
/** @brief STM32L1 SPI of Cat.1 and Cat.2, RM0038 28.2.1 and 28.5.2 FRF: no TI mode. */
inline constexpr spi_l1l4_variant l1_spi{.ti_mode = false, .kernel_buses = l1_apb_windows};
/** @brief STM32L4 SPI1 to SPI3, RM0351 42.3 Table 268: the TI mode and the FIFOs. */
inline constexpr spi_l1l4_variant l4_spi{
    .ti_mode      = true,
    .fifo         = true,
    .kernel_buses = l4_apb_windows};

/**
 * @brief Factory of the SPI model, RM0456 section 68 and RM0481 section 58.
 * @param devices Board `spi`, each instance's devices, copied when a model is made.
 * @param controllers Board `spi_controllers`, each instance's controller, shared.
 * @param variant What differs between the peripherals of the family the row serves.
 */
[[nodiscard]] auto spi_factory(
    decltype(board::spi) const&             devices,
    decltype(board::spi_controllers) const& controllers,
    spi_variant                             variant) -> model_factory;

/**
 * @brief Factory of the classic SPI model, RM0038 section 28.
 * @param devices Board `spi`, each instance's devices, copied when a model is made.
 * @param controllers Board `spi_controllers`, each instance's controller, shared.
 * @param variant What differs between the peripherals of the family the row serves.
 */
[[nodiscard]] auto spi_l1l4_factory(
    decltype(board::spi) const&             devices,
    decltype(board::spi_controllers) const& controllers,
    spi_l1l4_variant                        variant) -> model_factory;

/** @brief The STM32U5 and STM32H5 SPI model rows. */
[[nodiscard]] auto spi_u5h5_rows() -> std::span<builtin const>;

/** @brief The STM32L1 and STM32L4 SPI model rows. */
[[nodiscard]] auto spi_l1l4_rows() -> std::span<builtin const>;
}
