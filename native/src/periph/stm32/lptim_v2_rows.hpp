#pragma once

#include "row_of.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace popcorn::periph::stm32
{
/** @brief Trigger inputs `lptim_ext_trig0` to `lptim_ext_trig7` of one LPTIM, by TRIGSEL.
 *         Entry 0 is the instance's `<instance>_etr` pin. It has no default constructor, so a
 *         variant that omits its `triggers` field does not build. */
class lptim_v2_triggers
{
public:
  lptim_v2_triggers() = delete;

  constexpr lptim_v2_triggers(
      std::string_view trig0,
      std::string_view trig1,
      std::string_view trig2,
      std::string_view trig3,
      std::string_view trig4,
      std::string_view trig5,
      std::string_view trig6,
      std::string_view trig7)
      : names_{trig0, trig1, trig2, trig3, trig4, trig5, trig6, trig7}
  {}

  [[nodiscard]] constexpr auto operator[](std::size_t index) const -> std::string_view
  {
    return names_[index];
  }

private:
  std::array<std::string_view, 8> names_;
};

/** @brief One internal input of input capture 2, the RCC clock domain it divides. */
struct lptim_v2_clock_input
{
  /** @brief RCC clock domain, empty for a reserved input. */
  std::string_view domain;
  /** @brief Division of the domain's rate. */
  std::uint32_t divider = 1;
};

/** @brief Input capture 2 inputs of CFGR2 IC2SEL 01, 10 and 11, RM0456 Tables 608 to 610. IC2SEL
 *         00 is the instance's `<instance>_ch2` pin. It has no default constructor, so a variant
 *         that omits its `capture2_inputs` field does not build. */
class lptim_v2_capture2_inputs
{
public:
  lptim_v2_capture2_inputs() = delete;

  constexpr lptim_v2_capture2_inputs(
      lptim_v2_clock_input mux1,
      lptim_v2_clock_input mux2,
      lptim_v2_clock_input mux3)
      : inputs_{mux1, mux2, mux3}
  {}

  /** @brief The input of IC2SEL @p select, 1 to 3. */
  [[nodiscard]] constexpr auto operator[](std::size_t select) const -> lptim_v2_clock_input
  {
    return inputs_[select - 1];
  }

private:
  std::array<lptim_v2_clock_input, 3> inputs_;
};

/** @brief Differences between the LPTIM peripherals of the STM32U5 and STM32H5 IP the model rows
 *         serve, RM0456 58.3 Table 599. */
struct lptim_v2_variant
{
  /** @brief Name of the `LPTIMx_ISR` view the model claims, as the description spells it. */
  std::string_view status = "ISR_output";
  /** @brief Name of the `LPTIMx_ICR` view the model claims. */
  std::string_view clear = "ICR_output";
  /** @brief Name of the `LPTIMx_DIER` view the model claims. */
  std::string_view interrupts = "DIER_output";
  /** @brief Capture/compare channels: 2, or 0 on a reduced instance, which has no CCMR1 and
   *         CCR2. */
  std::uint8_t channels = 2;
  /** @brief The instance has the encoder mode, CFGR.ENC. */
  bool encoder = true;
  /** @brief The instance has the input capture mode, CCMR1.CC1SEL and CC2SEL. */
  bool capture = true;
  /** @brief RCC clock domain of the APB bus the instance sits on, which the RCR write crosses
   *         before the kernel clock domain. */
  std::string_view bus_clock = "pclk3";
  /** @brief Trigger inputs of TRIGSEL 0 to 7, RM0456 58.4.3 Table 604. */
  lptim_v2_triggers triggers;
  /** @brief Input capture 2 inputs of IC2SEL 01 to 11, RM0456 58.4.3 Tables 608 to 610. */
  lptim_v2_capture2_inputs capture2_inputs;
};

/** @brief STM32U5 LPTIM1 on APB3, RM0456 58.3 Table 599: encoder, input capture, two
 *         channels, LSI and LSE on input capture 2, Table 608. */
inline constexpr lptim_v2_variant u5_lptim1{
    .triggers =
        {"etr", "rtc_alra_trg", "rtc_alrb_trg", "tamp_trg1", "tamp_trg2", "lpdma_ch2_tc",
                   "comp1_out", "comp2_out"},
    .capture2_inputs = {{"lsi"}, {"lse"}, {}}
};
/** @brief STM32U5 LPTIM2: LPTIM1 on APB1, its clock enable in RCC_APB1ENR2, the GPDMA triggers of
 *         RM0456 Table 604, HSI16/256, MSIS/1024 and MSIS/4 on input capture 2, Table 609. */
inline constexpr lptim_v2_variant u5_lptim2{
    .bus_clock = "pclk1",
    .triggers =
        {"etr", "rtc_alra_trg", "rtc_alrb_trg", "tamp_trg1", "gpdma_ch0_tc", "gpdma_ch4_tc",
                   "comp1_out", "comp2_out"},
    .capture2_inputs = {{"hsi16", 256}, {"msis", 1024}, {"msis", 4}}
};
/** @brief STM32U5 LPTIM3: LPTIM1 without the encoder mode, RM0456 58.3 Table 599, and its own
 *         triggers of Table 604. */
inline constexpr lptim_v2_variant u5_lptim3{
    .encoder = false,
    .triggers =
        {"etr", "rtc_alra_trg", "rtc_alrb_trg", "tamp_trg1", "lpdma_ch1_tc", "tamp_trg3",
                   "comp1_out", "comp2_out"},
    .capture2_inputs = {{}, {}, {}}
};
/** @brief STM32U5 LPTIM4, RM0456 58.3 Table 599 and 58.7.1: no channel, no encoder, no input
 *         capture, the `ISR`, `ICR` and `DIER` layout of 58.7.1, 58.7.4 and 58.7.7. */
inline constexpr lptim_v2_variant u5_lptim4{
    .status     = "ISR",
    .clear      = "ICR",
    .interrupts = "DIER",
    .channels   = 0,
    .encoder    = false,
    .capture    = false,
    .triggers =
        {"etr", "rtc_alra_trg", "rtc_alrb_trg", "lpdma_ch0_tc", "tamp_trg2", "tamp_trg3",
                   "comp1_out", "comp2_out"},
    .capture2_inputs = {{}, {}, {}}
};

/**
 * @brief Factory of the LPTIM model of the STM32U5 and STM32H5 IP, RM0456 section 58.
 * @param variant What differs between the LPTIM instances the row serves.
 */
[[nodiscard]] auto lptim_v2_factory(lptim_v2_variant variant) -> model_factory;

/** @brief The LPTIM model rows of the STM32U5 and STM32H5 IP. */
[[nodiscard]] auto lptim_v2_rows() -> std::span<builtin const>;
}
