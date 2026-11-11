#pragma once

#include "row_of.hpp"

#include <popcorn/periph/stm32/board.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

namespace popcorn::periph::stm32
{
/** @brief Differences between the RTC and TAMP peripherals the model rows serve. */
struct rtc_variant
{
  /** @brief Name of the privilege register the security write handler binds. */
  std::string_view privilege_register = "PRIVCR";
};

/** @brief STM32U5 RTC and TAMP, RM0456 63.6.14 and 64.6.7. */
inline constexpr rtc_variant u5_rtc{};
/** @brief STM32H5 RTC and TAMP, RM0481 51.6.8 and 52.7.10. */
inline constexpr rtc_variant h5_rtc{.privilege_register = "PRIVCFGR"};

/** @brief Differences between the classic RTC peripherals the model rows serve, RM0038 20. */
struct rtc_l1l4_variant
{
  /** @brief Whether the RTC has subseconds and the shadow bypass: RTC_SSR, RTC_SHIFTR,
   *         RTC_TSSSR, RTC_ALRMASSR, RTC_ALRMBSSR, RTC_CR.BYPSHAD and RTC_ISR.SHPF. */
  bool subseconds = true;
  /** @brief Whether the RTC has smooth calibration: RTC_CALR and RTC_ISR.RECALPF. */
  bool smooth_calibration = true;
  /** @brief Whether the RTC has tamper inputs 2 and 3: TAMP2E, TAMP3E and their flags. */
  bool three_tampers = true;
  /** @brief Whether DBGMCU has DBG_RTC_STOP, so a held core can freeze the RTC. */
  bool debug_freeze = true;
  /** @brief Whether a brown-out resets the RTC domain as a power-on does, RM0038 6.1.2 and
   *         6.1.3 on a part with no VBAT supply. */
  bool brown_out_resets = true;
  /** @brief Whether ALRAWF and ALRBWF clear while their alarm is enabled and in init mode, and
   *         set up to 2 RTCCLK cycles after the alarm is disabled, RM0038 20.3.5 on Cat.1. Without
   *         it they read 1. */
  bool alarm_write_wait = false;
  /** @brief Name of the tamper control register: RTC_TAFCR on the STM32L1, RM0038 20.6.17,
   *         RTC_TAMPCR on the STM32L4, RM0351 38.6.16. */
  std::string_view tamper_register = "TAFCR";
  /** @brief Whether the RTC has coarse digital calibration: RTC_CALIBR and RTC_CR.DCE, RM0038
   *         20.6.7. */
  bool coarse_calibration = true;
  /** @brief Whether the RTC has RTC_OR with RTC_ALARM_TYPE and RTC_OUT_RMP, RM0351 38.6.19. */
  bool option_register = false;
  /** @brief Whether RTC_CR has ITSE, the timestamp on an internal event, RM0351 38.6.3. */
  bool internal_timestamp = false;
  /** @brief Whether TAMP2F and TAMP3F clear by a 0 under the RTC_WPR key only. RM0038 20.3.5
   *         exempts RTC_ISR[13:8] from the key, RM0351 38.3.7 exempts RTC_ISR[31:8]. */
  bool keyed_tamper_flags = true;
  /** @brief RTCCLK cycles from a shadow copy restart to the copy that sets RSF: every two RTCCLK
   *         periods in RM0038 20.3.2, every RTCCLK period in RM0351 38.3.4 and 38.3.8. */
  std::uint8_t shadow_copy_cycles = 2;
  /** @brief EXTI line of the alarm interrupt, RM0038 20.5. */
  std::uint8_t alarm_line = 17;
  /** @brief EXTI line of the wakeup interrupt, RM0038 20.5. */
  std::uint8_t wakeup_line = 20;
  /** @brief Whether the timestamp on the RTC_TS input is modelled: TSE, TSEDGE, TSF, TSOVF, the
   *         RTC_TSTR, RTC_TSDR and RTC_TSSSR capture and TSIE on the tamper line, RM0351 38.3.13.
   *         Without it a firmware TSE write logs once as storage, a model_error under
   *         strict_peripherals. */
  bool timestamps = false;
  /** @brief GPIO port of the RTC_TS input, RM0351 38.3.2. */
  std::string_view timestamp_port = "GPIOC";
  /** @brief Pin of the RTC_TS input in its port, RM0351 38.3.2. */
  std::uint8_t timestamp_pin = 13;
  /** @brief EXTI line of the tamper and timestamp interrupt, RM0038 20.5, RM0351 Table 62. */
  std::uint8_t tamper_line = 19;
  /** @brief Whether tamper detection on RTC_TAMP1 to RTC_TAMP3 is modelled with the RTC_TAMPCR
   *         field names of RM0351 38.6.16. Without it a firmware TAMPxE write logs once as
   *         storage, a model_error under strict_peripherals. */
  bool tampers = false;
  /** @brief GPIO ports of RTC_TAMP1 to RTC_TAMP3, RM0351 38.3.2 and DS10198 Table 16. */
  std::array<std::string_view, 3> tamper_ports{"GPIOC", "GPIOA", "GPIOE"};
  /** @brief Pins of RTC_TAMP1 to RTC_TAMP3 in their ports. */
  std::array<std::uint8_t, 3> tamper_pins{13, 0, 6};
};

/** @brief STM32L1 Cat.2 to Cat.6 RTC, `rtc2_v2_5_Cube`, RM0038 section 20. */
inline constexpr rtc_l1l4_variant l1_rtc{};
/** @brief STM32L1 Cat.1 RTC, RM0038 20.2, 20.3.1, 20.3.5 and 30.16.4: no subseconds, no smooth
 *         calibration, one tamper input, no DBG_RTC_STOP and alarm write flags that follow the
 *         alarm enables. */
inline constexpr rtc_l1l4_variant l1_cat1_rtc{
    .subseconds         = false,
    .smooth_calibration = false,
    .three_tampers      = false,
    .debug_freeze       = false,
    .alarm_write_wait   = true};
/** @brief STM32L4 RTC, `rtc2_v2_4_Cube`, RM0351 section 38: RTC_TAMPCR and RTC_OR, no
 *         RTC_CALIBR and no CR.DCE, CR.ITSE, RTC_ISR[31:8] outside the key, a shadow copy every
 *         RTCCLK period, a backup domain a brown-out keeps, RM0351 6.1.3, the alarm interrupt
 *         on EXTI line 18, RM0351 Table 62, the timestamp on PC13, RM0351 38.3.13, and the
 *         tampers on PC13, PA0 and PE6, RM0351 38.3.14. */
inline constexpr rtc_l1l4_variant l4_rtc{
    .brown_out_resets   = false,
    .tamper_register    = "TAMPCR",
    .coarse_calibration = false,
    .option_register    = true,
    .internal_timestamp = true,
    .keyed_tamper_flags = false,
    .shadow_copy_cycles = 1,
    .alarm_line         = 18,
    .timestamps         = true,
    .tampers            = true};

/**
 * @brief Factory of the RTC model, RM0456 section 63.
 * @param initial Calendar at power-on, the board's `rtc_initial`, copied.
 * @param variant What differs between the RTC of the family the row serves.
 */
[[nodiscard]] auto rtc_factory(std::optional<std::chrono::sys_seconds> initial, rtc_variant variant)
    -> model_factory;

/**
 * @brief Factory of the classic RTC model, RM0038 section 20.
 * @param initial Calendar at power-on, the board's `rtc_initial`, copied.
 * @param variant What differs between the RTC of the category the row serves.
 */
[[nodiscard]] auto
    rtc_l1l4_factory(std::optional<std::chrono::sys_seconds> initial, rtc_l1l4_variant variant)
        -> model_factory;

/**
 * @brief Factory of the TAMP backup register model, RM0456 section 64.
 * @param variant What differs between the TAMP of the family the row serves.
 */
[[nodiscard]] auto tamp_factory(rtc_variant variant) -> model_factory;

/** @brief The RTC and TAMP model rows. */
[[nodiscard]] auto rtc_rows() -> std::span<builtin const>;
}
