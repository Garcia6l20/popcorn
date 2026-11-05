#pragma once

#include "family_names.hpp"
#include "flash_unit.hpp"

#include <popcorn/periph/stm32/board.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string_view>

namespace popcorn::periph::stm32
{
/** @brief Differences between the STM32H5 HASH processors the model rows serve, RM0481 40.3.
 *         A code outside the variant's set reads 0000 after a CR write and the hash runs SHA-1,
 *         as read back on an STM32H573. */
struct hash_variant
{
  /** @brief ALGO also keeps the SHA-3 and SHAKE codes 0100 to 1001. */
  bool sha3 = false;
};

/** @brief STM32H52x, H53x, H56x and H57x, SHA-1 and SHA-2 only, RM0481 Table 397: ALGO keeps
 *         0000, 0010, 0011 and 1100 to 1111. */
inline constexpr hash_variant hash_h5_sha2{};
/** @brief STM32H543 and H553, SHA-3 and SHAKE too, RM0481 Table 397. */
inline constexpr hash_variant hash_h5_sha3{.sha3 = true};

/** @brief Differences between the IWDG peripherals the model rows serve. */
struct iwdg_variant
{
  /** @brief The peripheral has the window: `WINR` and `SR.WVU`. Without it a refresh reloads at
   *         any counter value. */
  bool window = true;
  /** @brief The peripheral has the early wake-up interrupt: `EWCR`, `SR.EWU`, `SR.EWIF` and the
   *         `IWDG_IRQn` line. */
  bool early_wakeup = true;
  /** @brief Largest `PR` value with its own divider, 4 << `PR`. A larger value divides by
   *         4 << `prescaler_top`. */
  std::uint8_t prescaler_top = 8;
  /** @brief LSI cycles from a `PR`, `RLR`, `WINR` or `EWCR` write to the fall of its update
   *         flag. */
  std::uint8_t update_cycles = 6;
  /**
   * @brief `FLASH` register and field of the hardware watchdog option. At every reset the
   *        watchdog starts when the field reads 0. Empty: it starts only at `KR` 0xCCCC.
   */
  field_name hardware_option = {};
};

/** @brief STM32U5 and STM32H5 IWDG, RM0456 61.7.2 and Table 623, RM0481 49.7.2. */
inline constexpr iwdg_variant u5_iwdg{};
/** @brief STM32L1 IWDG, RM0038 21.4: KR, PR, RLR and SR with PVU and RVU only, PR 110 and 111
 *         both divide by 256, an update takes 5 LSI cycles. */
inline constexpr iwdg_variant l1_iwdg{
    .window        = false,
    .early_wakeup  = false,
    .prescaler_top = 6,
    .update_cycles = 5};
/**
 * @brief STM32L4 IWDG, RM0351 36.4: KR, PR, RLR, SR with PVU, RVU and WVU, and WINR, no early
 *        wake-up, PR 110 and 111 both divide by 256, an update takes up to 5 LSI cycles, the
 *        hardware watchdog started by FLASH_OPTR.IWDG_SW 0, RM0351 36.3.3.
 */
inline constexpr iwdg_variant l4_iwdg{
    .window          = true,
    .early_wakeup    = false,
    .prescaler_top   = 6,
    .update_cycles   = 5,
    .hardware_option = {"OPTR", "IWDG_SW"}
};

/** @brief Differences between the DAC peripherals the model rows serve. */
struct dac_variant
{
  /** @brief DAC_CR.TSELx value that selects the software trigger SWTRIGx. */
  std::uint32_t software_trigger = 0b111;
  /** @brief Trigger signal of each DAC_CR.TSELx value below @ref software_trigger. */
  std::array<std::string_view, 7> hardware_triggers{
      "tim6_trgo", "tim8_trgo", "tim7_trgo", "tim5_trgo", "tim2_trgo", "tim4_trgo", "exti9"};
  /** @brief Factory OTRIMx of each channel, the DAC_CCR reset value. RM0351 19.7.15 gives
   *         0x00XX 00XX, so the value is a mid-range code, inferred. */
  std::array<std::uint32_t, 2> factory_trim{16, 16};
  /** @brief GPIO port of the channel output pads, RM0351 19.3. */
  std::string_view pad_port = "GPIOA";
  /** @brief Pin of each channel's DAC_OUTx pad on @ref pad_port, PA4 and PA5, RM0351 19.3. */
  std::array<std::uint8_t, 2> pads{4, 5};
};

/** @brief STM32L4 DAC1, RM0351 section 19, the triggers and SWTRIG at TSELx 111 of RM0351
 *         Table 128. */
inline constexpr dac_variant l4_dac{};

/** @brief Differences between the CRC peripherals the model rows serve. */
struct crc_variant
{
  /** @brief The unit has CRC_INIT, CRC_POL and the CR fields POLYSIZE, REV_IN and REV_OUT.
   *         Without them the unit computes CRC-32 over the polynomial 0x04C11DB7 from
   *         0xFFFFFFFF, with no reversal. */
  bool programmable = true;
  /** @brief A CRC_DR write narrower than 32 bits feeds the word its byte lanes repeat, such as
   *         0xB0B0B0B0 for the byte 0xB0, logs once and raises model_error under strict
   *         peripherals. Otherwise a right-aligned narrow write feeds its own width. */
  bool word_input = false;
  /** @brief A byte or half-word access to CRC_IDR is served as is, the CMSIS device header
   *         declaring it a byte. Otherwise a narrow access to it is a forbidden access. */
  bool byte_idr = false;
  /** @brief Manual section that states the access rules, named in the log records. */
  std::string_view access_rule = "RM0456 24.4";
  /** @brief Manual section that leaves even polynomials unsupported, named in the log record. */
  std::string_view polynomial_rule = "RM0456 24.3.3";
};

/** @brief STM32U5 and STM32H5 CRC, RM0456 section 24. */
inline constexpr crc_variant u5_crc{};
/** @brief STM32L1 CRC, RM0038 section 4: DR, IDR and CR.RESET only, word accesses, the CMSIS
 *         `CRC_TypeDef` declaring IDR an `uint8_t`. */
inline constexpr crc_variant l1_crc{
    .programmable = false,
    .word_input   = true,
    .byte_idr     = true,
    .access_rule  = "RM0038 4.4"};
/** @brief STM32L4 CRC, RM0351 section 15: the programmable unit of the STM32U5 with an 8-bit
 *         CRC_IDR, which the CMSIS `CRC_TypeDef` declares an `uint8_t`. */
inline constexpr crc_variant l4_crc{
    .byte_idr        = true,
    .access_rule     = "RM0351 15.4",
    .polynomial_rule = "RM0351 15.3.3"};

/** @brief Differences between the RNG peripherals the model rows serve. */
struct rng_variant
{
  /** @brief The unit has the configuration block of RM0456 48.7: CR.CONDRST, CONFIGLOCK,
   *         RNG_CONFIG1 to RNG_CONFIG3, NISTC, CLKDIV and ARDIS, NSCR and HTCR, the seed error
   *         auto-reset, the 4-word FIFO in front of the 128-bit conditioning register and the
   *         RM0456 48.5 timing. Without it CR holds IE, RNGEN and CED, DR holds one word, a
   *         word takes word_cycles RNG clock cycles and the first after RNGEN start_cycles. */
  bool configurable = true;
  /** @brief CECS is 1 while clock_ratio x fRNG < fHCLK. */
  std::uint8_t clock_ratio = 32;
  /** @brief RNG clock cycles from RNGEN 1 to the first word, without the configuration block. */
  std::uint8_t start_cycles = 0;
  /** @brief RNG clock cycles from an empty DR to the next word, without the configuration
   *         block. */
  std::uint8_t word_cycles = 0;
};

/** @brief STM32U5 and STM32H5 RNG, RM0456 section 48. */
inline constexpr rng_variant u5_rng{};
/** @brief STM32L4 RNG, RM0351 section 27: CR with IE, RNGEN and CED, a word every 42 RNG clock
 *         cycles, the first 46 cycles after RNGEN, 27.5, and a clock error below fHCLK / 16,
 *         27.3.3. */
inline constexpr rng_variant l4_rng{
    .configurable = false,
    .clock_ratio  = 16,
    .start_cycles = 46,
    .word_cycles  = 42};

/** @brief Differences between the STM32L1 ADC categories the model rows serve, RM0038 12.3.3. */
struct adc_l1_variant
{
  /** @brief The unit has the two channel banks of ADC_CR2.ADC_CFG, ADC_SMPR0 for channels 30
   *         and 31 and the 28th rank SQ28. Without them it has one bank and 27 ranks. */
  bool banks = true;
  /** @brief External channels of bank A, bit n for ADC_INn, RM0038 Table 58. */
  std::uint32_t external = 0xFBFC'FFFF;
  /** @brief Bank B channels on pads of their own, bit n for ADC_INnb, RM0038 Table 41. The other
   *         bank B channels share the pad of their bank A channel. */
  std::uint32_t bank_b_only = 0x0000'1FCF;
  /** @brief Address of the factory VREFINT_CAL halfword. */
  std::uint32_t vrefint_cal = 0x1FF8'00F8;
  /** @brief Address of the factory TS_CAL1 halfword, the sensor code at 30 C. */
  std::uint32_t ts_cal1 = 0x1FF8'00FA;
};

/** @brief STM32L1 Cat.3 to Cat.6 ADC, DS8890, DS10262, DS10764 and DS10002. */
inline constexpr adc_l1_variant l1_adc{};
/** @brief STM32L1 Cat.1 and Cat.2 ADC, `STM32L1_adc_cat12`: one bank of ADC_IN0 to ADC_IN25,
 *         27 ranks, DS6876 and DS9515. */
inline constexpr adc_l1_variant l1_cat12_adc{
    .banks       = false,
    .external    = 0x03FC'FFFF,
    .bank_b_only = 0,
    .vrefint_cal = 0x1FF8'0078,
    .ts_cal1     = 0x1FF8'007A};

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
struct rtc_classic_variant
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
inline constexpr rtc_classic_variant l1_rtc{};
/** @brief STM32L1 Cat.1 RTC, RM0038 20.2, 20.3.1, 20.3.5 and 30.16.4: no subseconds, no smooth
 *         calibration, one tamper input, no DBG_RTC_STOP and alarm write flags that follow the
 *         alarm enables. */
inline constexpr rtc_classic_variant l1_cat1_rtc{
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
inline constexpr rtc_classic_variant l4_rtc{
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

/** @brief Differences between the SAI peripherals the model rows serve. */
struct sai_variant
{
  /** @brief Name of the block enable field of `xCR1`. */
  std::string_view enable = "SAIAEN";
  /** @brief Whether `xCR1` has OSR, the master clock oversampling ratio. */
  bool oversampling = true;
  /** @brief Whether the SAI has PDMCR and PDMDLY, the PDM interface. */
  bool pdm = true;
  /**
   * @brief Whether MCKDIV above 0 divides the kernel clock by 2 x MCKDIV and NODIV 1 bypasses
   *        it, RM0351 43.3.8. Otherwise MCKDIV divides by MCKDIV, 0 counting as 1, in both
   *        NODIV settings.
   */
  bool doubled_divider = false;
};

/** @brief STM32U5 SAI, RM0456 69.6.2. */
inline constexpr sai_variant u5_sai{};
/** @brief STM32H5 SAI, RM0481 59.6.2. */
inline constexpr sai_variant h5_sai{.enable = "SAIEN"};
/** @brief STM32L4 SAI, RM0351 43.5: GCR to BDR, SAIEN enables a block, MCKDIV doubled. */
inline constexpr sai_variant l4_sai{
    .enable          = "SAIEN",
    .oversampling    = false,
    .pdm             = false,
    .doubled_divider = true};

}
