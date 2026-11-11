#pragma once

#include "row_of.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

namespace popcorn::periph::stm32
{
/** @brief One code of a `SYSCFG_EXTICRn` selector field. */
struct exti_port_code
{
  /** @brief Field value. */
  std::uint8_t code;
  /** @brief Port letter. */
  char port;
  /** @brief Lines that take the port at this code, bit n for line n. */
  std::uint16_t lines = 0xFFFF;
};

/** @brief Differences between the classic SYSCFG controllers the model rows serve, by register
 *         role. A role the family lacks is empty. */
struct syscfg_l1l4_variant
{
  /** @brief Selector codes. A code with no entry, or an entry without the line, selects no
   *         port. */
  std::span<exti_port_code const> ports;
  /** @brief Register of the USB D+ pull-up enable. */
  std::string_view usb_pull_up_register;
  /** @brief Field of the USB D+ pull-up enable. The other fields of its register are storage
   *         with the storage diagnostics. */
  std::string_view usb_pull_up_field;
  /** @brief Peer told of every change of the pull-up enable through usb_pull_up_listener. */
  std::string_view usb_peer;
  /** @brief Registers a SYSCFGRST pulse keeps. */
  std::span<std::string_view const> kept_registers;
  /** @brief Registers served as storage the model does not model, with the storage
   *         diagnostics. */
  std::span<std::string_view const> storage_registers;
  /** @brief Register of the SRAM2 software erase, empty on a family without one. */
  std::string_view erase_register = {};
  /** @brief Field of erase_register that starts the erase. */
  std::string_view erase_field = {};
  /** @brief Read-only field of erase_register set while the erase runs. */
  std::string_view busy_field = {};
  /** @brief Register of the key sequence that unlocks erase_field. */
  std::string_view key_register = {};
  /** @brief Memory region the erase writes 0 to, by its description name. */
  std::string_view erase_region = {};
};

/** @brief RM0038 8.5.3 to 8.5.6, Cat.3 to Cat.6: PA to PE, PH for lines 0 to 2 only, PF and PG. */
inline constexpr auto l1_exti_ports = std::to_array<exti_port_code>({
    {0, 'A'},
    {1, 'B'},
    {2, 'C'},
    {3, 'D'},
    {4, 'E'},
    {5, 'H', 0x0007},
    {6, 'F'},
    {7, 'G'},
});
/** @brief STM32L1 storage registers of SYSCFG: MEMRMP. */
inline constexpr auto l1_syscfg_storage = std::to_array<std::string_view>({"MEMRMP"});
/** @brief STM32L1 SYSCFG registers a SYSCFGRST pulse keeps, RM0038 8.5.1: MEMRMP. */
inline constexpr auto l1_syscfg_kept = std::to_array<std::string_view>({"MEMRMP"});
/** @brief STM32L1 Cat.3 to Cat.6, RM0038 section 8.5. */
inline constexpr syscfg_l1l4_variant l1_syscfg{
    .ports                = l1_exti_ports,
    .usb_pull_up_register = "PMC",
    .usb_pull_up_field    = "USB_PU",
    .usb_peer             = "USB",
    .kept_registers       = l1_syscfg_kept,
    .storage_registers    = l1_syscfg_storage};
/** @brief RM0351 9.2.3 to 9.2.6, STM32L47x/L48x: PA to PG on every line, PH for lines 0 and 1
 *         only, 3-bit fields. */
inline constexpr auto l4_exti_ports = std::to_array<exti_port_code>({
    {0, 'A'},
    {1, 'B'},
    {2, 'C'},
    {3, 'D'},
    {4, 'E'},
    {5, 'F'},
    {6, 'G'},
    {7, 'H', 0x0003},
});
/** @brief STM32L47x/L48x storage registers of SYSCFG: MEMRMP, CFGR1, CFGR2, SWPR. */
inline constexpr auto l4_syscfg_storage =
    std::to_array<std::string_view>({"MEMRMP", "CFGR1", "CFGR2", "SWPR"});
/** @brief STM32L47x/L48x SYSCFG registers a SYSCFGRST pulse keeps, inferred from the "System
 *         reset value" of RM0351 9.2.7 to 9.2.10 and "cleared only by a system reset" of 9.2.8
 *         and 9.2.9: MEMRMP, SCSR, CFGR2, SWPR. */
inline constexpr auto l4_syscfg_kept =
    std::to_array<std::string_view>({"MEMRMP", "SCSR", "CFGR2", "SWPR"});
/** @brief STM32L47x/L48x, RM0351 section 9.2: EXTI selection, the SRAM2 erase through SCSR and
 *         SKR, no USB pull-up. */
inline constexpr syscfg_l1l4_variant l4_syscfg{
    .ports                = l4_exti_ports,
    .usb_pull_up_register = "",
    .usb_pull_up_field    = "",
    .usb_peer             = "",
    .kept_registers       = l4_syscfg_kept,
    .storage_registers    = l4_syscfg_storage,
    .erase_register       = "SCSR",
    .erase_field          = "SRAM2ER",
    .busy_field           = "SRAM2BSY",
    .key_register         = "SKR",
    .erase_region         = "SRAM2"};
/** @brief STM32L1 Cat.1 and Cat.2, `STM32L1_exti_23`, RM0038 8.5.3: codes 0110 and 0111, PF and
 *         PG, exist on Cat.3 to Cat.6 only. */
inline constexpr syscfg_l1l4_variant l1_syscfg_cat12{
    .ports                = std::span{l1_exti_ports}.first(6),
    .usb_pull_up_register = "PMC",
    .usb_pull_up_field    = "USB_PU",
    .usb_peer             = "USB",
    .kept_registers       = l1_syscfg_kept,
    .storage_registers    = l1_syscfg_storage};

/** @brief Factory of the SYSCFG model, RM0456 section 15. */
[[nodiscard]] auto syscfg_u5_factory() -> model_factory;

/**
 * @brief Factory of the classic SYSCFG model, RM0038 section 8.5.
 * @param variant Register roles and EXTI port codes of the family the row serves.
 */
[[nodiscard]] auto syscfg_l1l4_factory(syscfg_l1l4_variant variant) -> model_factory;

/** @brief The STM32L1 and STM32L4 SYSCFG model rows. */
[[nodiscard]] auto syscfg_l1l4_rows() -> std::span<builtin const>;

/** @brief The STM32U5 SYSCFG model rows. */
[[nodiscard]] auto syscfg_u5_rows() -> std::span<builtin const>;
}
