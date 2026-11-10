#pragma once

#include "row_of.hpp"

#include <popcorn/periph/stm32/board.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace popcorn::periph::stm32
{
/** @brief Trigger signals of DAC_CR.TSELx 000 to 110 on the STM32L4, RM0351 Table 128. */
inline constexpr auto l4_dac_triggers = std::to_array<std::string_view>(
    {"tim6_trgo", "tim8_trgo", "tim7_trgo", "tim5_trgo", "tim2_trgo", "tim4_trgo", "exti9"});

/**
 * @brief Trigger signals of DAC_CR.TSELx 000 to 110 on the STM32L1, RM0038 Table 68. TSELx 001 is
 *        reserved, its entry is empty.
 */
inline constexpr auto l1_dac_triggers = std::to_array<std::string_view>(
    {"tim6_trgo", "", "tim7_trgo", "tim9_trgo", "tim2_trgo", "tim4_trgo", "exti9"});

/**
 * @brief Trigger signals of DAC_CR.TSELx 0001 to 1111 on the STM32U5, RM0456 Table 342. Entries
 *        without a source in the table are reserved and empty.
 */
inline constexpr auto u5_dac_triggers = std::to_array<std::string_view>(
    {"tim1_trgo", "tim2_trgo", "tim4_trgo", "tim5_trgo", "tim6_trgo", "tim7_trgo", "tim8_trgo",
     "tim15_trgo", "", "", "lptim1_ch1", "lptim3_ch1", "exti9", "", ""});

/**
 * @brief Trigger signals of DAC_CR.TSELx 0001 to 1111 on the STM32H5, RM0481 Table 306. trg14 and
 *        trg15 are the PLA outputs of the STM32H533 and STM32H543 only and stay reserved.
 */
inline constexpr auto h5_dac_triggers = std::to_array<std::string_view>(
    {"tim1_trgo", "tim2_trgo", "tim4_trgo", "tim5_trgo", "tim6_trgo", "tim7_trgo", "tim8_trgo",
     "tim15_trgo", "", "", "lptim1_ch1", "lptim2_ch1", "exti9", "", ""});

/** @brief Trigger sources the model knows by name and no model produces. */
inline constexpr auto unproduced_triggers =
    std::to_array<std::string_view>({"lptim1_ch1", "lptim2_ch1", "lptim3_ch1"});

/** @brief Differences between the DAC peripherals the model rows serve. */
struct dac_variant
{
  /** @brief DAC_CR.TSELx value that selects the software trigger SWTRIGx. */
  std::uint32_t software_trigger = 0b111;
  /** @brief Smallest DAC_CR.TSELx value that selects an entry of @ref hardware_triggers. */
  std::uint32_t first_hardware = 0;
  /** @brief Trigger signal of each hardware DAC_CR.TSELx value, from @ref first_hardware. An empty
   *         name is a reserved value. */
  std::span<std::string_view const> hardware_triggers = l4_dac_triggers;
  /** @brief Trigger signals of @ref hardware_triggers that no model produces: a channel that
   *         selects one with TENx set never triggers. */
  std::span<std::string_view const> unproduced = {};
  /** @brief Clock domain of the digital interface, the `dac_hclk` cycles of RM0456 35.4.6 on the
   *         STM32U5 and STM32H5, the APB1 clock on the STM32L1 and STM32L4. */
  std::string_view bus_clock = "pclk1";
  /** @brief Prefix of the DMA request names, `<prefix>_ch<x>_dma`. Empty: the lower-case
   *         instance name. */
  std::string_view dma_prefix = {};
  /** @brief The peripheral has `DACxRDY` in `DAC_SR`: ENx set starts the wake-up, and a data
   *         write or software trigger before DACxRDY is refused, RM0456 35.4.4. */
  bool ready_flags = false;
  /** @brief The peripheral has `SINFORMATx`, the signed input format of RM0456 35.4.5. */
  bool signed_format = false;
  /** @brief The peripheral has `DMADOUBLEx` and the `DACCxDHRB` fields of the holding registers. */
  bool double_data = false;
  /** @brief Picoseconds from ENx set to DACxRDY with the output buffer on, the typical tWAKEUP of
   *         the datasheet, inferred. */
  std::uint64_t wakeup_buffer_ps = 0;
  /** @brief The same with the output buffer off. */
  std::uint64_t wakeup_no_buffer_ps = 0;
  /** @brief Bit mask over DAC_MCR.MODEx of the values with the output buffer off. */
  std::uint8_t buffer_off_modes = 0;
  /** @brief Bit mask over DAC_MCR.MODEx of the reserved values: a write of one keeps the old
   *         MODEx. */
  std::uint8_t reserved_modes = 0;
  /** @brief Factory OTRIMx of each channel, the DAC_CCR reset value. RM0351 19.7.15 gives
   *         0x00XX 00XX, so the value is a mid-range code, inferred. */
  std::array<std::uint32_t, 2> factory_trim{16, 16};
  /** @brief GPIO port of the channel output pads, RM0351 19.3. */
  std::string_view pad_port = "GPIOA";
  /** @brief Pin of each channel's DAC_OUTx pad on @ref pad_port, PA4 and PA5, RM0351 19.3. */
  std::array<std::uint8_t, 2> pads{4, 5};
  /** @brief Name of the software trigger register. */
  std::string_view swtrig_register = "SWTRGR";
  /** @brief Name of the interrupt line. */
  std::string_view irq = "TIM6_DAC_IRQn";
  /** @brief The peripheral has the self-calibration: `CENx`, `DAC_CCR` and `CAL_FLAGx`. */
  bool calibration = true;
  /** @brief The peripheral has `DAC_MCR`. Without it the output is on the pad whenever ENx is 1
   *         and never on chip. */
  bool mode_register = true;
  /** @brief The peripheral has the sample and hold mode: `DAC_SHSRx`, `DAC_SHHR`, `DAC_SHRR`
   *         and `BWSTx`. */
  bool sample_hold = true;
  /** @brief Clearing DMAENx leaves a DMA request that is high pending until a DMA transfer
   *         acknowledges it, ES0125 2.6.1 and the errata sheets of the other STM32L1 lines. */
  bool dmaen_keeps_request = false;
  /** @brief Bit mask over DAC_MCR.MODEx of the values that drive the pad. */
  std::uint8_t pad_modes = 0b0111'0111;
  /** @brief Bit mask over DAC_MCR.MODEx of the values that drive the on-chip peripherals. */
  std::uint8_t chip_modes = 0b1110'1010;

  /**
   * @brief Index into @ref hardware_triggers of a DAC_CR.TSELx value.
   * @param select The TSELx value.
   * @return The index, empty for the software trigger or a value past the table.
   */
  [[nodiscard]] constexpr auto hardware_index(std::uint32_t select) const
      -> std::optional<std::uint32_t>
  {
    if(select == software_trigger
       or select < first_hardware
       or select - first_hardware >= hardware_triggers.size())
    {
      return std::nullopt;
    }
    return select - first_hardware;
  }
};

/** @brief STM32L4 DAC1, RM0351 section 19, the triggers and SWTRIG at TSELx 111 of RM0351
 *         Table 128. */
inline constexpr dac_variant l4_dac{};

/**
 * @brief STM32L1 DAC, RM0038 section 13: no self-calibration, no `DAC_MCR`, no sample and hold,
 *        `DAC_SWTRIGR`, the interrupt line `DAC_IRQn`, the triggers of Table 68. The output is on
 *        the pad whenever ENx is 1.
 */
inline constexpr dac_variant l1_dac{
    .hardware_triggers   = l1_dac_triggers,
    .swtrig_register     = "SWTRIGR",
    .irq                 = "DAC_IRQn",
    .calibration         = false,
    .mode_register       = false,
    .sample_hold         = false,
    .dmaen_keeps_request = true};

/**
 * @brief STM32U5 DAC1, RM0456 section 35: TSELx 0000 is SWTRIGx and 0001 to 1111 select
 *        dac_chx_trg1 to trg15, the digital interface runs on the AHB clock, DACxRDY, signed
 *        format, the MODEx table of RM0456 35.7.16 like the STM32L4.
 */
inline constexpr dac_variant u5_dac{
    .software_trigger    = 0,
    .first_hardware      = 1,
    .hardware_triggers   = u5_dac_triggers,
    .unproduced          = unproduced_triggers,
    .bus_clock           = "hclk",
    .dma_prefix          = "dac1",
    .ready_flags         = true,
    .signed_format       = true,
    .double_data         = true,
    .wakeup_buffer_ps    = 4'200'000,
    .wakeup_no_buffer_ps = 2'000'000,
    .buffer_off_modes    = 0b1100'1100,
    .irq                 = "DAC1_IRQn"};

/**
 * @brief STM32H5 DAC1, RM0481 section 30: the U5 layout, no on-chip modes, MODEx 001, 011, 101
 *        and 111 reserved, the pad driven in 000, 010, 100 and 110.
 */
inline constexpr dac_variant h5_dac{
    .software_trigger    = 0,
    .first_hardware      = 1,
    .hardware_triggers   = h5_dac_triggers,
    .unproduced          = unproduced_triggers,
    .bus_clock           = "hclk",
    .dma_prefix          = "dac1",
    .ready_flags         = true,
    .signed_format       = true,
    .double_data         = true,
    .wakeup_buffer_ps    = 5'000'000,
    .wakeup_no_buffer_ps = 2'000'000,
    .buffer_off_modes    = 0b0100'0100,
    .reserved_modes      = 0b1010'1010,
    .irq                 = "DAC1_IRQn",
    .pad_modes           = 0b0101'0101,
    .chip_modes          = 0};

/**
 * @brief Factory of the DAC model, RM0351 section 19.
 * @param supply VDDA, the VREF+ the channel outputs scale to.
 * @param variant What differs between the DAC of the family the row serves.
 */
[[nodiscard]] auto dac_factory(supply_options supply, dac_variant variant) -> model_factory;

/** @brief The DAC model rows. */
[[nodiscard]] auto dac_rows() -> std::span<builtin const>;
}
