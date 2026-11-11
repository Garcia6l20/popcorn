#pragma once

#include "row_of.hpp"

#include <popcorn/periph/stm32/board.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

namespace popcorn::periph::stm32
{
/** @brief Differences between the ADC4 units the model rows serve, `aditf4_v4_U5_Cube`. */
struct adc4_variant
{
  /** @brief ADC_CCR.PRESC dividers of the codes 0000 to 1011, RM0456 34.7.18. The codes above
   *         are reserved. */
  std::array<std::uint16_t, 12> prescalers{1, 2, 4, 6, 8, 10, 12, 16, 32, 64, 128, 256};
  /** @brief tADCVREG_STUP, the LDO startup ready flag time, in nanoseconds, the maximum of
   *         DS13737 Table 106 and DS13086 Table 108. */
  std::uint64_t regulator_setup_ns = 25'000;
  /** @brief tOFF_CAL, the offset calibration time, in ADC clock cycles, the typical value of
   *         DS13737 Table 106 and DS13086 Table 108. */
  std::uint32_t calibration_cycles = 123;
  /** @brief tSTAB in ADC clock cycles, 3 cycles and one conversion of DS13737 Table 106 and
   *         DS13086 Table 108, the conversion taken as 12 bits at SMP 000, 1.5 + 12.5 cycles. */
  std::uint32_t stabilisation_cycles = 17;
  /** @brief ADC clock cycles from the ADDIS write to ADEN and ADDIS clearing, ES0499 2.9.2. */
  std::uint32_t disable_cycles = 6;
  /** @brief ADC clock cycles after ADCAL clears during which an ADEN write is lost, RM0456
   *         34.4.5. */
  std::uint32_t enable_lockout_cycles = 4;
  /** @brief External channels, bit n for ADC4_INn, RM0456 Figure 283. */
  std::uint32_t external = 0x00DF'8FFE;
  /** @brief Channels ADC_CFGR1.AWD1CH can name, bit n for channel n, RM0456 34.7.4. The other
   *         codes are reserved. */
  std::uint32_t awd1_channels = 0x00C3'FFFF;
  /** @brief The channel VIN[n] of VREFINT with VREFEN, RM0456 Table 329. */
  std::uint32_t vrefint_channel = 0;
  /** @brief The channel of VCORE, RM0456 Table 329. */
  std::uint32_t vcore_channel = 12;
  /** @brief The channel of VSENSE with VSENSESEL, RM0456 Table 329. */
  std::uint32_t sensor_channel = 13;
  /** @brief The channel of VBAT / 4 with VBATEN, RM0456 Table 329. */
  std::uint32_t battery_channel = 14;
  /** @brief The channel of the DAC1 output ADC_OR.CHN21SEL selects, RM0456 Table 329. */
  std::uint32_t dac_channel = 21;
  /** @brief Address of the factory TS_CAL1 halfword, 14 bits at 30 C, RM0456 Table 325. */
  std::uint32_t ts_cal1_address = 0x0BFA'0710;
  /** @brief Address of the factory VREFINT_CAL halfword, 14 bits at 30 C, RM0456 Table 326. */
  std::uint32_t vrefint_cal_address = 0x0BFA'07A5;
  /** @brief VDDA of the factory words in millivolts, RM0456 Tables 325 and 326. */
  std::uint32_t factory_vdda_mv = 3000;
  /** @brief Typical sampling time of VREFINT in nanoseconds, the tS_vrefint of DS13737 Table 36,
   *         its minimum is not given. */
  std::uint64_t vrefint_sampling_ns = 12'650;
  /** @brief Shortest sampling time of VSENSE in nanoseconds, the tS_temp of DS13737 Table 109. */
  std::uint64_t sensor_sampling_ns = 13'000;
  /** @brief Sampling times of the SMPx codes 000 to 111 in half ADC clock cycles, RM0456 34.7.6. */
  std::array<std::uint16_t, 8> sampling_halves{3, 7, 15, 25, 39, 79, 159, 1629};
  /** @brief tSAR of RES 12, 10, 8 and 6 bits in half ADC clock cycles, RM0456 Table 332. */
  std::array<std::uint16_t, 4> sar_halves{25, 21, 17, 13};
  /** @brief Quarter ADC clock cycles from an ADSTART write to the start of sampling, the tLATR
   *         of DS13737 Table 106 and DS13086 Table 108: 4 cycles at fADC = HCLK and HCLK / 2. */
  std::uint32_t start_latency_quarters = 16;
  /** @brief The same at fADC = HCLK / 4, 3.75 cycles. */
  std::uint32_t start_latency_quarters_div4 = 15;
  /** @brief Signals of adc_trg0 to adc_trg7 selected by EXTSEL, RM0456 Table 329. A timer output
   *         compare event is the timer's `_cc` capture/compare event signal. */
  std::array<std::string_view, 8> trigger_signals{"tim1_trgo2", "tim1_cc4",  "tim2_trgo",
                                                  "tim15_trgo", "tim6_trgo", "lptim1_ch1",
                                                  "lptim3_ch2", "exti15"};
  /** @brief ADC clock cycles ADC_CFGR2.LFTRIG adds to the trigger latency, RM0456 34.4.22. */
  std::uint32_t rearm_cycles = 2;
  /** @brief Largest ADC_CFGR2.OVSS shift, RM0456 34.7.5. */
  std::uint32_t max_oversampling_shift = 8;
  /** @brief Lowest ADC clock in Hz, fADC of DS13737 Table 106 and DS13086 Table 108. */
  std::uint64_t min_hz = 140'000;
  /** @brief Highest ADC clock in Hz, fADC of DS13737 Table 106 and DS13086 Table 108. */
  std::uint64_t max_hz = 55'000'000;
};

/** @brief The ADC4 of the STM32U575 and STM32U585, RM0456 section 34. */
inline constexpr adc4_variant u5_adc4{};

/**
 * @brief Factory of the ADC4 model, RM0456 section 34.
 * @param supply Board supplies, VDDA the conversion reference, copied into every model made.
 * @param variant What differs between the ADC4 units the row serves.
 */
[[nodiscard]] auto adc4_factory(supply_options supply, adc4_variant variant) -> model_factory;

/** @brief The ADC4 model rows. */
[[nodiscard]] auto adc4_rows() -> std::span<builtin const>;
}
