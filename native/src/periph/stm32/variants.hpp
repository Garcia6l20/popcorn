#pragma once

#include "bus_window.hpp"
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

/** @brief Differences between the I2C peripherals the model rows serve. */
struct i2c_variant
{
  /** @brief The peripheral has the `I2C_AUTOCR` register and its trigger and DMA fields. */
  bool autocr = true;
  /** @brief CR1 has ADDRACLR and STOPFACLR, which keep ADDR and STOPF clear. */
  bool clear_on_ack = true;
  /**
   * @brief A write of ICR.ADDRCF clears CR2.START, RM0351 39.9.2 and 39.9.8 as corrected by
   *        ES0250 2.20.7. Otherwise an address matched in target mode clears it, RM0456 65.9.2.
   */
  bool start_cleared_by_address_clear = false;
};

/** @brief STM32U5 I2C, RM0456 65.7.14. */
inline constexpr i2c_variant u5_i2c{};
/** @brief STM32H5 I2C, RM0481 has no `I2C_AUTOCR`. */
inline constexpr i2c_variant h5_i2c{.autocr = false};
/** @brief STM32L4 I2C1 to I2C3, RM0351 39.9: no `I2C_AUTOCR`, no ADDRACLR and STOPFACLR, the
 *         kernel clock the CCIPR I2CxSEL selection. */
inline constexpr i2c_variant l4_i2c{
    .autocr                         = false,
    .clear_on_ack                   = false,
    .start_cleared_by_address_clear = true};

/** @brief Differences between the I2C peripherals of RM0038 section 26 the model rows serve. */
struct i2c_v1_variant
{
  /** @brief Buses whose clock is the clock of an instance in their window. */
  std::span<bus_window const> kernel_buses;
};

/** @brief STM32L1 I2C1 and I2C2 on APB1, RM0038 Table 5. */
inline constexpr i2c_v1_variant l1_i2c{.kernel_buses = l1_apb_windows};

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
    .mode_views         = false,
    .fifo               = false,
    .names              = sci3_names,
    .break_keeps_tc_low = true};
/**
 * @brief STM32L4 LPUART1, RM0351 41.7: the L4 USART registers with BRR[19:0], no OVER8 and no
 *        RQR.TXFRQ, the kernel clock the CCIPR.LPUART1SEL selection.
 */
inline constexpr usart_variant l4_lpuart{
    .mode_views         = false,
    .fifo               = false,
    .names              = sci3_lpuart_names,
    .break_keeps_tc_low = true};

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
struct spi_classic_variant
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
inline constexpr spi_classic_variant l1_spi_ti{.ti_mode = true, .kernel_buses = l1_apb_windows};
/** @brief STM32L1 SPI of Cat.1 and Cat.2, RM0038 28.2.1 and 28.5.2 FRF: no TI mode. */
inline constexpr spi_classic_variant l1_spi{.ti_mode = false, .kernel_buses = l1_apb_windows};
/** @brief STM32L4 SPI1 to SPI3, RM0351 42.3 Table 268: the TI mode and the FIFOs. */
inline constexpr spi_classic_variant l4_spi{
    .ti_mode      = true,
    .fifo         = true,
    .kernel_buses = l4_apb_windows};

/** @brief Names a USB full-speed device controller gives the register fields whose spelling
 *         differs between the variants. An empty name is a field the variant lacks. */
struct usb_drd_names
{
  /** @brief Endpoint register prefix, the register of endpoint n is `<prefix><n>R`. */
  std::string_view endpoint;
  /** @brief Transmit status, toggled by a written 1. */
  std::string_view stat_tx;
  /** @brief Transmit data toggle, toggled by a written 1. */
  std::string_view dtog_tx;
  /** @brief Transmit transfer done, cleared by a written 0. */
  std::string_view done_tx;
  /** @brief Endpoint kind. */
  std::string_view kind;
  /** @brief Endpoint type. */
  std::string_view type;
  /** @brief Receive status, toggled by a written 1. */
  std::string_view stat_rx;
  /** @brief Receive data toggle, toggled by a written 1. */
  std::string_view dtog_rx;
  /** @brief Receive transfer done, cleared by a written 0. */
  std::string_view done_rx;
  /** @brief ISTR endpoint identifier. */
  std::string_view endpoint_id;
  /** @brief ISTR bus reset flag. */
  std::string_view reset_done;
  /** @brief CNTR forced reset. */
  std::string_view core_reset;
  /** @brief CNTR suspend. */
  std::string_view suspend;
  /** @brief CNTR suspend ready or low-power mode. */
  std::string_view suspend_ready;
  /** @brief CNTR LPM L1 resume request. */
  std::string_view l1_resume;
  /** @brief CNTR L2 resume request. */
  std::string_view l2_resume;
  /** @brief CNTR host mode. */
  std::string_view host_mode;
};

/** @brief Differences between the USB full-speed device controllers the usb_drd rows serve. */
struct usb_drd_variant
{
  /** @brief Field names. */
  usb_drd_names names;
  /** @brief Endpoint register fields cleared by a written 0. */
  std::span<std::string_view const> endpoint_clears;
  /** @brief ISTR fields cleared by a written 0. */
  std::span<std::string_view const> status_clears;
  /** @brief NVIC line of every enabled ISTR flag. */
  std::string_view irq;
  /** @brief NVIC line of a transfer done on an isochronous or double-buffered bulk endpoint,
   *         whatever CNTR.CTRM holds, empty for none. */
  std::string_view priority_irq;
  /** @brief LPM register, empty for none. */
  std::string_view lpm_register;
  /** @brief Battery charging detection register, empty for none. */
  std::string_view charging_register;
  /** @brief Register of the D+ pull-up enable inside the controller, empty when a peer gives
   *         it. */
  std::string_view pull_up_register;
  /** @brief Field of the D+ pull-up enable inside the controller. */
  std::string_view pull_up_field;
  /** @brief Peer whose usb_pull_up_source gives the pull-up enable, with no pull-up register. */
  std::string_view pull_up_peer;
  /** @brief Register holding the buffer descriptor table address, its field of the same name,
   *         empty for a table at packet memory address 0. */
  std::string_view descriptor_register;
  /** @brief Peripheral whose window is the packet memory, empty for a window inside the
   *         controller's own block. */
  std::string_view pma_peripheral;
  /** @brief Offset of the packet memory window in its peripheral. */
  std::uint32_t pma_offset;
  /** @brief Packet memory bytes as the controller addresses them. */
  std::uint32_t pma_bytes;
  /** @brief Packet memory bytes behind each 32-bit word of the CPU window, from its low lane
   *         up. */
  std::uint32_t pma_unit;
  /** @brief Bits of a buffer address the controller uses. */
  std::uint32_t address_mask;
  /** @brief CPU access sizes the manual allows on the packet memory, bit n for n bytes. */
  std::uint8_t pma_sizes;
  /** @brief EXTI line that follows ISTR.WKUP, none for no line. */
  std::optional<std::uint8_t> wakeup_line;
  /** @brief Whether the hardware owns the suspend bit: a written 0 keeps it, setting it clears
   *         FNR.LSOF, the ready bit follows it, a wake-up clears both. Otherwise the firmware owns
   *         it and a wake-up clears the low-power bit only. */
  bool hardware_suspend;
  /** @brief Whether the forced reset acts as a bus reset: endpoints and DADDR cleared and the
   *         bus reset flag set. Otherwise it clears the endpoints only. */
  bool forced_bus_reset;
  /** @brief Manual section of the registers, for configuration errors. */
  std::string_view manual;
};

inline constexpr auto h5_usb_endpoint_clears = std::to_array<std::string_view>(
    {"VTTX", "VTRX", "NAK", "ERR_TX", "ERR_RX", "THREE_ERR_TX", "THREE_ERR_RX"});
inline constexpr auto h5_usb_status_clears = std::to_array<std::string_view>(
    {"L1REQ", "ESOF", "SOF", "RST_DCON", "SUSP", "WKUP", "ERR", "PMAOVR", "THR512", "DDISC"});
/** @brief STM32H5 USB, `usbfs_v1`, RM0481 section 61: CHEPnR, 2048 bytes of packet memory at
 *         offsets 0x400 to 0xBFF read by word, the descriptor table at address 0, the pull-up in
 *         BCDR, LPM, battery charging detection and host mode. */
inline constexpr usb_drd_variant h5_usb{
    .names =
        {.endpoint      = "CHEP",
                .stat_tx       = "STATTX",
                .dtog_tx       = "DTOGTX",
                .done_tx       = "VTTX",
                .kind          = "EPKIND",
                .type          = "UTYPE",
                .stat_rx       = "STATRX",
                .dtog_rx       = "DTOGRX",
                .done_rx       = "VTRX",
                .endpoint_id   = "IDN",
                .reset_done    = "RST_DCON",
                .core_reset    = "USBRST",
                .suspend       = "SUSPEN",
                .suspend_ready = "SUSPRDY",
                .l1_resume     = "L1RES",
                .l2_resume     = "L2RES",
                .host_mode     = "HOST"},
    .endpoint_clears     = h5_usb_endpoint_clears,
    .status_clears       = h5_usb_status_clears,
    .irq                 = "USB_DRD_FS_IRQn",
    .priority_irq        = "",
    .lpm_register        = "LPMCSR",
    .charging_register   = "BCDR",
    .pull_up_register    = "BCDR",
    .pull_up_field       = "DPPU_DPD",
    .pull_up_peer        = "",
    .descriptor_register = "",
    .pma_peripheral      = "",
    .pma_offset          = 0x400,
    .pma_bytes           = 0x800,
    .pma_unit            = 4,
    .address_mask        = 0xFFFC,
    .pma_sizes           = 1u << 4,
    .wakeup_line         = std::nullopt,
    .hardware_suspend    = true,
    .forced_bus_reset    = false,
    .manual              = "RM0481 61.6"
};

inline constexpr auto l1_usb_endpoint_clears =
    std::to_array<std::string_view>({"CTR_TX", "CTR_RX"});
inline constexpr auto l1_usb_status_clears =
    std::to_array<std::string_view>({"ESOF", "SOF", "RESET", "SUSP", "WKUP", "ERR", "PMAOVR"});
/** @brief STM32L1 USB, `usb1_v1_2_STM32L1_Cube`, RM0038 section 24: EPnR, 512 bytes of packet
 *         memory as 256 half-words, each in the low half of a word of the `USB_SRAM` window,
 *         RM0038 24.5.3, read by half-word or word, the descriptor table at BTABLE, the pull-up
 *         in SYSCFG_PMC.USB_PU, RM0038 24.4.2, USB_LP and USB_HP, RM0038 24.3.1, ISTR.WKUP on
 *         EXTI line 18, RM0038 Table 51. No LPM, battery charging detection or host mode. */
inline constexpr usb_drd_variant l1_usb{
    .names =
        {.endpoint      = "EP",
                .stat_tx       = "STAT_TX",
                .dtog_tx       = "DTOG_TX",
                .done_tx       = "CTR_TX",
                .kind          = "EP_KIND",
                .type          = "EP_TYPE",
                .stat_rx       = "STAT_RX",
                .dtog_rx       = "DTOG_RX",
                .done_rx       = "CTR_RX",
                .endpoint_id   = "EP_ID",
                .reset_done    = "RESET",
                .core_reset    = "FRES",
                .suspend       = "FSUSP",
                .suspend_ready = "LPMODE",
                .l1_resume     = "",
                .l2_resume     = "RESUME",
                .host_mode     = ""},
    .endpoint_clears     = l1_usb_endpoint_clears,
    .status_clears       = l1_usb_status_clears,
    .irq                 = "USB_LP_IRQn",
    .priority_irq        = "USB_HP_IRQn",
    .lpm_register        = "",
    .charging_register   = "",
    .pull_up_register    = "",
    .pull_up_field       = "",
    .pull_up_peer        = "SYSCFG",
    .descriptor_register = "BTABLE",
    .pma_peripheral      = "USB_SRAM",
    .pma_offset          = 0,
    .pma_bytes           = 0x200,
    .pma_unit            = 2,
    .address_mask        = 0xFFFE,
    .pma_sizes           = (1u << 2) | (1u << 4),
    .wakeup_line         = 18,
    .hardware_suspend    = false,
    .forced_bus_reset    = true,
    .manual              = "RM0038 24.5"
};

/** @brief Differences between the OTG_FS cores the otg_fs rows serve. */
struct otg_fs_variant
{
  /** @brief GRSTCTL bit 2, the frame counter reset. */
  std::string_view frame_counter_reset;
  /** @brief GRSTCTL bit 1, the partial soft reset, empty for none. */
  std::string_view partial_reset;
  /** @brief GPWRDN and GADPCTL present, claimed as storage registers. */
  bool adp;
};

/** @brief STM32U5 OTG_FS, GRSTCTL in RM0456 72.15.5. */
inline constexpr otg_fs_variant u5_otg_fs{
    .frame_counter_reset = "FSRST",
    .partial_reset       = "PSRST",
    .adp                 = false};

/** @brief STM32L47x/L48x OTG_FS, GRSTCTL in RM0351 47.15.5, no PSRST per Table 322. */
inline constexpr otg_fs_variant l4_otg_fs{
    .frame_counter_reset = "FCRST",
    .partial_reset       = "",
    .adp                 = true};
}
