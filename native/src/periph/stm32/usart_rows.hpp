#pragma once

#include "bus_window.hpp"
#include "family_names.hpp"
#include "row_of.hpp"

#include <popcorn/periph/stm32/board.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

namespace popcorn::periph::stm32
{
/** @brief Register set of a USART IP version. */
enum class usart_registers : std::uint8_t
{
  /**
   * @brief `sci3`: ISR, ICR, RQR, RDR, TDR and PRESC, FIFO flags and thresholds, TEACK, REACK
   *        and BUSY, RM0456 66.8.
   */
  sci3,
  /**
   * @brief `sci2`: SR with `rc_w0` flags, one DR for both directions, SBK in CR1, the word length
   *        in CR1.M, BRR as DIV_Mantissa and DIV_Fraction, RM0038 27.6.
   */
  sci2,
};

/** @brief Fields of a USART whose name differs between register sets and families. */
enum class usart_role : std::uint8_t
{
  tx_not_full,
  rx_not_empty,
  noise,
  tx_not_full_ie,
  rx_not_empty_ie,
  tx_empty,
  rx_full,
  tx_empty_ie,
  rx_full_ie,
  tx_threshold,
  rx_threshold,
  tx_threshold_ie,
  rx_threshold_ie,
  tx_threshold_flag,
  rx_threshold_flag,
  flush_tx,
  prescaler,
  noise_clear,
};

/**
 * @brief sci3 names with the FIFO, RM0456 66.8 and 67.7, RM0481 56.8 and 57.7. `CR1` and `ISR`
 *        stand for the register or the view the model uses, such as `CR1_enabled`.
 */
inline constexpr std::array<named_field<usart_role>, role_count<usart_role>> sci3_fifo_names{
    {{usart_role::tx_not_full, {"ISR", "TXFNF"}},
     {usart_role::rx_not_empty, {"ISR", "RXFNE"}},
     {usart_role::noise, {"ISR", "NE"}},
     {usart_role::tx_not_full_ie, {"CR1", "TXFNFIE"}},
     {usart_role::rx_not_empty_ie, {"CR1", "RXFNEIE"}},
     {usart_role::tx_empty, {"ISR", "TXFE"}},
     {usart_role::rx_full, {"ISR", "RXFF"}},
     {usart_role::tx_empty_ie, {"CR1", "TXFEIE"}},
     {usart_role::rx_full_ie, {"CR1", "RXFFIE"}},
     {usart_role::tx_threshold, {"CR3", "TXFTCFG"}},
     {usart_role::rx_threshold, {"CR3", "RXFTCFG"}},
     {usart_role::tx_threshold_ie, {"CR3", "TXFTIE"}},
     {usart_role::rx_threshold_ie, {"CR3", "RXFTIE"}},
     {usart_role::tx_threshold_flag, {"ISR", "TXFT"}},
     {usart_role::rx_threshold_flag, {"ISR", "RXFT"}},
     {usart_role::flush_tx, {"RQR", "TXFRQ"}},
     {usart_role::prescaler, {"PRESC", "PRESCALER"}},
     {usart_role::noise_clear, {"ICR", "NECF"}}}
};

/**
 * @brief sci3 names of the STM32L4 USART and UART, RM0351 40.8: one data register each way, no
 *        FIFO and no PRESC, TXE, RXNE and NF in ISR, NCF in ICR.
 */
inline constexpr std::array<named_field<usart_role>, role_count<usart_role>> sci3_names{
    {{usart_role::tx_not_full, {"ISR", "TXE"}},
     {usart_role::rx_not_empty, {"ISR", "RXNE"}},
     {usart_role::noise, {"ISR", "NF"}},
     {usart_role::tx_not_full_ie, {"CR1", "TXEIE"}},
     {usart_role::rx_not_empty_ie, {"CR1", "RXNEIE"}},
     {usart_role::tx_empty, {"", ""}},
     {usart_role::rx_full, {"", ""}},
     {usart_role::tx_empty_ie, {"", ""}},
     {usart_role::rx_full_ie, {"", ""}},
     {usart_role::tx_threshold, {"", ""}},
     {usart_role::rx_threshold, {"", ""}},
     {usart_role::tx_threshold_ie, {"", ""}},
     {usart_role::rx_threshold_ie, {"", ""}},
     {usart_role::tx_threshold_flag, {"", ""}},
     {usart_role::rx_threshold_flag, {"", ""}},
     {usart_role::flush_tx, {"RQR", "TXFRQ"}},
     {usart_role::prescaler, {"", ""}},
     {usart_role::noise_clear, {"ICR", "NCF"}}}
};

/** @brief sci3 names of the STM32L4 LPUART1, RM0351 41.7: the USART names with no RQR.TXFRQ. */
inline constexpr std::array<named_field<usart_role>, role_count<usart_role>> sci3_lpuart_names{
    {{usart_role::tx_not_full, {"ISR", "TXE"}},
     {usart_role::rx_not_empty, {"ISR", "RXNE"}},
     {usart_role::noise, {"ISR", "NF"}},
     {usart_role::tx_not_full_ie, {"CR1", "TXEIE"}},
     {usart_role::rx_not_empty_ie, {"CR1", "RXNEIE"}},
     {usart_role::tx_empty, {"", ""}},
     {usart_role::rx_full, {"", ""}},
     {usart_role::tx_empty_ie, {"", ""}},
     {usart_role::rx_full_ie, {"", ""}},
     {usart_role::tx_threshold, {"", ""}},
     {usart_role::rx_threshold, {"", ""}},
     {usart_role::tx_threshold_ie, {"", ""}},
     {usart_role::rx_threshold_ie, {"", ""}},
     {usart_role::tx_threshold_flag, {"", ""}},
     {usart_role::rx_threshold_flag, {"", ""}},
     {usart_role::flush_tx, {"", ""}},
     {usart_role::prescaler, {"", ""}},
     {usart_role::noise_clear, {"ICR", "NCF"}}}
};

/** @brief sci2 names, RM0038 27.6: `ISR` stands for SR. */
inline constexpr std::array<named_field<usart_role>, role_count<usart_role>> sci2_names{
    {{usart_role::tx_not_full, {"ISR", "TXE"}},
     {usart_role::rx_not_empty, {"ISR", "RXNE"}},
     {usart_role::noise, {"ISR", "NF"}},
     {usart_role::tx_not_full_ie, {"CR1", "TXEIE"}},
     {usart_role::rx_not_empty_ie, {"CR1", "RXNEIE"}},
     {usart_role::tx_empty, {"", ""}},
     {usart_role::rx_full, {"", ""}},
     {usart_role::tx_empty_ie, {"", ""}},
     {usart_role::rx_full_ie, {"", ""}},
     {usart_role::tx_threshold, {"", ""}},
     {usart_role::rx_threshold, {"", ""}},
     {usart_role::tx_threshold_ie, {"", ""}},
     {usart_role::rx_threshold_ie, {"", ""}},
     {usart_role::tx_threshold_flag, {"", ""}},
     {usart_role::rx_threshold_flag, {"", ""}},
     {usart_role::flush_tx, {"", ""}},
     {usart_role::prescaler, {"", ""}},
     {usart_role::noise_clear, {"", ""}}}
};

/** @brief Differences between the USART, UART and LPUART peripherals the model rows serve. */
struct usart_variant
{
  /** @brief The peripheral has the `CR1_enabled` and `ISR_enabled` register views and no `CR1`
   *         and `ISR`. */
  bool mode_views = true;
  /** @brief The peripheral has the `FIFOEN` control bit, otherwise it holds one data word. */
  bool fifo = true;
  /** @brief Name of the IRQ line, empty for `<instance>_IRQn`. */
  std::string_view irq_name = {};
  /** @brief Register set of the IP version. */
  usart_registers registers = usart_registers::sci3;
  /**
   * @brief Buses whose clock is the kernel clock of an instance in their window. Empty: the RCC
   *        model names the kernel clock after the instance, such as `usart1`.
   */
  std::span<bus_window const> kernel_buses = {};
  /**
   * @brief High time after a break before the next frame, in half bits: 2 bits on `sci3`,
   *        RM0456 66.5.7, 1 bit on `sci2`, RM0038 27.3.2.
   */
  std::uint8_t break_stop_half_bits = 4;
  /**
   * @brief The instance has CTS and RTS flow control. Without it CTSE and RTSE are logged once
   *        and leave transmission and reception unchanged.
   */
  bool flow_control = true;
  /** @brief Names of the fields that differ between register sets, checked by
   *         names_every_role() and, against `fifo`, by flag_matches_roles(). */
  std::span<named_field<usart_role> const> names = sci3_fifo_names;
  /**
   * @brief A break requested while a data frame shifts with CTSE 1 keeps TC 0 at that frame's
   *        end when nCTS is deasserted then, ES0250 2.21.2 and 2.22.1 on the STM32L4, ES0125
   *        2.11.3, ES0242 2.11.3, ES0224 2.10.3, ES0240 and ES0241 2.12.3 and ES0578 2.11.3 on
   *        the STM32L1. Without it TC sets at the end of the data frame, RM0456 66.8.10.
   */
  bool break_keeps_tc_low = false;
  /**
   * @brief With CTSE 1 a break leaves at the end of the current transmission whatever nCTS says,
   *        ES0125 2.11.8, ES0242 2.11.8, ES0224 2.10.8, ES0240 and ES0241 2.12.8 and ES0578
   *        2.11.8 on the STM32L1. Without it nCTS holds a break as it holds a data frame, RM0456
   *        66.5.21.
   */
  bool break_ignores_cts = false;
  /**
   * @brief Reference manual named in the UE-lock warning, such as "RM0351". Only rows whose
   *        locks are built read it: an `sci2` row never does.
   */
  std::string_view manual = "RM0456";
  /** @brief CLKEN is written only with UE 0, RM0351 40.8.2. */
  bool clken_locked = false;
  /**
   * @brief ADD is written with UE 1 while RE is 0, RM0351 40.8.2 and 41.7.2. Without it ADD
   *        changes only with UE 0, RM0456 66.8.3 and 67.7.3.
   */
  bool add_open_with_receiver_off = false;
};

/** @brief STM32U5 USART and UART, RM0456 66.8, and every STM32H5 USART and UART, RM0481 56.8. */
inline constexpr usart_variant with_views_usart{};
/** @brief STM32U5 LPUART, RM0456 67.7, one `CR1` and one `ISR`. */
inline constexpr usart_variant u5_lpuart{.mode_views = false};
/** @brief STM32H5 LPUART, RM0481 57.7: the register views of the USART, and the CMSIS header
 *         names its line `LPUART1_IRQn` for the peripheral `LPUART`. */
inline constexpr usart_variant h5_lpuart{.irq_name = "LPUART1_IRQn"};
/** @brief STM32L1 USART and UART, RM0038 27.6, clocked by the APB bus of their address. */
inline constexpr usart_variant l1_usart{
    .mode_views           = false,
    .fifo                 = false,
    .registers            = usart_registers::sci2,
    .kernel_buses         = l1_apb_windows,
    .break_stop_half_bits = 2,
    .names                = sci2_names,
    .break_keeps_tc_low   = true,
    .break_ignores_cts    = true};
/** @brief STM32L1 UART4 and UART5: the L1 USART with no flow control, RM0038 Table 157. */
inline constexpr usart_variant l1_uart{
    .mode_views           = false,
    .fifo                 = false,
    .registers            = usart_registers::sci2,
    .kernel_buses         = l1_apb_windows,
    .break_stop_half_bits = 2,
    .flow_control         = false,
    .names                = sci2_names};
/**
 * @brief STM32L4 USART1 to USART3, UART4 and UART5, RM0351 40.8: one `CR1` and one `ISR`, the
 *        sci3 registers with one data register each way and no PRESC. Every instance has flow
 *        control, RM0351 Table 251. The kernel clock is the CCIPR selection of the instance.
 */
inline constexpr usart_variant l4_usart{
    .mode_views                 = false,
    .fifo                       = false,
    .names                      = sci3_names,
    .break_keeps_tc_low         = true,
    .manual                     = "RM0351",
    .clken_locked               = true,
    .add_open_with_receiver_off = true};
/**
 * @brief STM32L4 LPUART1, RM0351 41.7: the L4 USART registers with BRR[19:0], no OVER8 and no
 *        RQR.TXFRQ, the kernel clock the CCIPR.LPUART1SEL selection.
 */
inline constexpr usart_variant l4_lpuart{
    .mode_views                 = false,
    .fifo                       = false,
    .names                      = sci3_lpuart_names,
    .break_keeps_tc_low         = true,
    .manual                     = "RM0351",
    .add_open_with_receiver_off = true};

/**
 * @brief Factory of the USART, UART and LPUART model, RM0456 sections 66 and 67.
 * @param uarts Board `uarts`, each instance's far end, copied: the endpoints stay shared.
 * @param variant What differs between the peripherals of the family the row serves.
 */
[[nodiscard]] auto usart_factory(decltype(board::uarts) const& uarts, usart_variant variant)
    -> model_factory;

/** @brief The USART, UART and LPUART model rows. */
[[nodiscard]] auto usart_rows() -> std::span<builtin const>;
}
