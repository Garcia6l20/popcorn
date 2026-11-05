#pragma once

#include <popcorn/periph/stm32/board.hpp>

#include "row_of.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

namespace popcorn::periph::stm32
{
/** @brief Prefixes of the per-pin field names of a GPIO port, the pin number appended. */
struct gpio_field_names
{
  /** @brief `GPIOx_MODER` mode field. */
  std::string_view mode = "MODE";
  /** @brief `GPIOx_OTYPER` output type field. */
  std::string_view type = "OT";
  /** @brief `GPIOx_OSPEEDR` speed field. */
  std::string_view speed = "OSPEED";
  /** @brief `GPIOx_PUPDR` pull field. */
  std::string_view pull = "PUPD";
  /** @brief `GPIOx_IDR` input field. */
  std::string_view input = "ID";
  /** @brief `GPIOx_ODR` output field. */
  std::string_view output = "OD";
  /** @brief `GPIOx_AFRL` alternate function field of pins 0 to 7. */
  std::string_view alternate_low = "AFSEL";
  /** @brief `GPIOx_AFRH` alternate function field of pins 8 to 15. */
  std::string_view alternate_high = "AFSEL";
};

/**
 * @brief Differences between the GPIO ports of the families a model row serves.
 *        The model throws at attach when the description disagrees.
 */
struct gpio_variant
{
  /** @brief Port with fewer than 16 pins, 0 for none. */
  char short_port = 0;
  /** @brief Pins of that port. Its registers carry the per-pin fields of these pins only. */
  std::uint8_t short_port_pins = 16;
  /** @brief The port has `GPIOx_SECCFGR`, the TrustZone pin security register. */
  bool secure = true;
  /** @brief The port has `GPIOx_HSLVR`, the high-speed low-voltage register. */
  bool slew = true;
  /** @brief The port has `GPIOx_BRR`, the bit reset register. */
  bool brr = true;
  /** @brief The port has `GPIOx_ASCR`, the analog switch control register. */
  bool analog_switch = false;
  /** @brief Port whose pins the independent I/O supply VDDIO2 powers, 0 for none. */
  char supply_port = 0;
  /** @brief Pins of that port VDDIO2 powers, bit n for pin n. */
  std::uint16_t supply_pins = 0;
  /** @brief Per-pin field names. */
  gpio_field_names names = {};
};

/** @brief STM32U5 ports with 16 pins, RM0456 13.4.10. */
inline constexpr gpio_variant u5_gpio{};
/** @brief STM32U575 and STM32U585 ports, port I has pins 0 to 7, read back on a NUCLEO-U575ZI-Q:
 *         `GPIOI_MODER` keeps 0x0000FFFF of 0xFFFFFFFF and `GPIOI_AFRH` reads 0, RM0456 13.4.1. */
inline constexpr gpio_variant u575_gpio{.short_port = 'I', .short_port_pins = 8};
/** @brief STM32H56x and H57x ports, port I has pins 0 to 11, read back on an STM32H573:
 *         `GPIOI_MODER` keeps 0x00FFFFFF of 0xFFFFFFFF and `GPIOI_AFRH` holds AFSEL8 to AFSEL11,
 *         as DS14121 Table 17 lists. */
inline constexpr gpio_variant h56x_gpio{.short_port = 'I', .short_port_pins = 12};

/** @brief STM32L1 per-pin field names, RM0038 7.4: MODERy, OSPEEDRy, PUPDRy, IDRy, ODRy, AFRLy
 *         and AFRHy. */
inline constexpr gpio_field_names l1_gpio_names{
    .mode           = "MODER",
    .speed          = "OSPEEDR",
    .pull           = "PUPDR",
    .input          = "IDR",
    .output         = "ODR",
    .alternate_low  = "AFRL",
    .alternate_high = "AFRH",
};
/** @brief STM32L1 Cat.1 to Cat.3 ports, `STM32L1_gpio_cat13`, RM0038 7.4: no `GPIOx_BRR`, which
 *         RM0038 7.4.11 gives to Cat.4, Cat.5 and Cat.6 only, no TrustZone and no HSLV. */
inline constexpr gpio_variant l1_gpio_cat13{
    .secure = false,
    .slew   = false,
    .brr    = false,
    .names  = l1_gpio_names};
/** @brief STM32L1 Cat.4 to Cat.6 ports, `STM32L1_gpio_cat46`, RM0038 7.4 and 7.4.11. */
inline constexpr gpio_variant l1_gpio_cat46{
    .secure = false,
    .slew   = false,
    .brr    = true,
    .names  = l1_gpio_names};

/** @brief STM32L47x and L48x ports, `STM32L47x_gpio_v1_0`, RM0351 8.4: `GPIOx_BRR` and
 *         `GPIOx_ASCR`, no TrustZone and no HSLV, PG2 to PG15 on VDDIO2, RM0351 5.4.2. */
inline constexpr gpio_variant l4_gpio{
    .secure        = false,
    .slew          = false,
    .brr           = true,
    .analog_switch = true,
    .supply_port   = 'G',
    .supply_pins   = 0xFFFC,
    .names         = l1_gpio_names};

/**
 * @brief Factory of the GPIO port model, RM0456 section 13 and RM0038 section 7. Returns no model
 *        for LPGPIO1.
 * @param variant What differs between the ports of the family the row serves.
 */
[[nodiscard]] auto gpio_factory(gpio_variant variant) -> model_factory;

/** @brief The GPIO model rows. */
[[nodiscard]] auto gpio_rows() -> std::span<builtin const>;
}
