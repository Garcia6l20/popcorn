#pragma once

#include "row_of.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace popcorn::periph::stm32
{
/** @brief The option register field value under which a trigger_entry holds. */
struct trigger_remap
{
  /** @brief Register of the field, such as `OR`. */
  std::string_view reg;
  /** @brief Field that selects between the inputs of one TS value, such as `ITR1_RMP`. */
  std::string_view field;
  /** @brief Field value that selects this entry's source. */
  std::uint32_t value;
};

/** @brief One internal trigger input of a timer's slave mode controller. */
struct trigger_entry
{
  /** @brief SMCR.TS value that selects the input. */
  std::uint32_t select;
  /** @brief Source whose `<source>_trgo` the input carries: a timer, such as `tim2`, the OC1
   *         output of a timer, such as `tim10_oc1`, which that timer drives from OC1 as
   *         `tim10_oc1_trgo`, or a source no model drives, such as `usbsof`. An OC1 source counts
   *         up only, its CR1 has no DIR and no CMS. */
  std::string_view source;
  /** @brief Instance whose slave mode controller has the input, empty for every timer. */
  std::string_view slave{};
  /** @brief Option register field value the input needs, empty when TS alone selects it. */
  std::optional<trigger_remap> remap{};
};

/** @brief A timer whose tim_etr_in is a pin signal, with no ETRSEL field. */
struct etr_pin
{
  /** @brief Instance, such as `TIM3`. */
  std::string_view instance;
  /** @brief Pin signal that carries ETR, such as `tim3_etr`. */
  std::string_view signal;
};

/** @brief A timer whose one IRQ has a name other than `<instance>_IRQn`. */
struct tim_irq
{
  /** @brief Instance, such as `TIM6`. */
  std::string_view instance;
  /** @brief IRQ name as the description spells it, such as `TIM6_DAC_IRQn`. */
  std::string_view irq;
  /** @brief Group line the name replaces, `UP`, `CC`, `BRK` or `TRG_COM`, empty for the one line
   *         of a single-line timer. */
  std::string_view group{};
};

/** @brief The input_remap channel of a field that selects tim_etr_in. */
inline constexpr std::uint8_t tim_etr_input = 4;

/** @brief An option register field that connects a timer input to its pin or to an internal
 *         source. */
struct input_remap
{
  /** @brief Instance that has the field. */
  std::string_view slave;
  /** @brief Input the field selects for, 0 to 3 for tim_ti1 to tim_ti4, `tim_etr_input` for
   *         tim_etr_in on a timer the tim_variant lists in `etr_pins`. */
  std::uint8_t channel;
  /** @brief Register of the field, such as `OR`. */
  std::string_view reg;
  /** @brief Field name, such as `TI1_RMP`. */
  std::string_view field;
  /** @brief Bit v set when field value v keeps the input on its pin, value 0 alone by
   *         default. */
  std::uint32_t pin_values = 1;
  /** @brief The internal source a value outside `pin_values` selects is ORed with the pin, so
   *         the input keeps following its pin. Unset, the internal source replaces it. */
  bool ored = false;
};

/** @brief Differences between the timer peripherals the model rows serve. */
struct tim_variant
{
  /** @brief Internal trigger inputs by TS value. The input a timer would take from itself is
   *         reserved, and TS values with no entry reach nothing. */
  std::span<trigger_entry const> triggers;
  /** @brief Instances whose CR2.MMS has 4 bits, bit 3 in field `MMS_1`. The other timers with an
   *         MMS field have 3 bits. */
  std::span<std::string_view const> wide_master;
  /** @brief Instances with no master mode, so no CR2.MMS field. Every other timer has one. */
  std::span<std::string_view const> no_master;
  /** @brief SMCR.SMS has its high bit in field `SMS_1`. Otherwise SMS is 3 bits wide and the field
   *         is absent. */
  bool slave_high = true;
  /** @brief SMCR.TS has its high bits in field `TS_1`. Otherwise TS is 3 bits wide and the field
   *         is absent. */
  bool select_high = true;
  /** @brief CR1 has `UIFREMAP`, which copies UIF into `CNT.UIFCPY` where CNT has that field.
   *         Otherwise neither field exists. */
  bool uif_remap = true;
  /** @brief While CR1.UIFREMAP is 1, a read of a 32-bit CNT with no `UIFCPY` field gives UIF in
   *         bit 31 in place of the count's bit 31, RM0351 31.3.16, RM0456 55.4.20 and RM0481
   *         44.4.20. Unset, bit 31 holds the count. */
  bool uif_overwrites_count = false;
  /** @brief Instances whose tim_etr_in is a pin signal, with no ETRSEL field. The other timers
   *         have ETR only through `AF1.ETRSEL`. */
  std::span<etr_pin const> etr_pins{};
  /** @brief When set, the instances that count up only: CR1 has no DIR and no CMS, and SMS 001
   *         to 011 are reserved. Every other instance of the variant has DIR and CMS. Unset, DIR
   *         and CMS are what the description gives. */
  std::optional<std::span<std::string_view const>> up_only{};
  /** @brief Option register fields that select a timer input's source. A value the entry's
   *         `pin_values` lacks gives an internal source. */
  std::span<input_remap const> input_remaps{};
  /** @brief Instances whose IRQ shares its line with another peripheral and carries its name.
   *         The other timers raise `<instance>_IRQn`, or the `_UP`, `_CC`, `_BRK` and
   *         `_TRG_COM` group lines. */
  std::span<tim_irq const> irq_names{};
};

/** @brief RM0456 54.6.2 and 55.5.2, RM0481 43.6.2 and 44.5.2: MMS[3:0] on TIM1, TIM8 and TIM2 to
 *         TIM5. RM0456 56.7.2, 57.4.2 and RM0481 45.4.2, 46.7.2, 47.7.2: MMS[2:0] on the others. */
inline constexpr auto wide_master_timers =
    std::to_array<std::string_view>({"TIM1", "TIM2", "TIM3", "TIM4", "TIM5", "TIM8"});

/** @brief TS 0 to 3 are ITR0 to ITR3, 8 to 10 are ITR4 to ITR6, RM0456 54.6.3 and 55.5.3 TIMx_SMCR.
 */
inline constexpr auto u5_triggers = std::to_array<trigger_entry>({
    {0,  "tim1" },
    {1,  "tim2" },
    {2,  "tim3" },
    {3,  "tim4" },
    {8,  "tim5" },
    {9,  "tim8" },
    {10, "tim15"},
});
/** @brief RM0456 56.8.2: TIM16 and TIM17 have no master mode. */
inline constexpr auto u5_no_master = std::to_array<std::string_view>({"TIM16", "TIM17"});
/** @brief STM32U5 timers. */
inline constexpr tim_variant u5_tim{
    .triggers             = u5_triggers,
    .wide_master          = wide_master_timers,
    .no_master            = u5_no_master,
    .uif_overwrites_count = true};

/**
 * @brief TS 0 to 3 are ITR0 to ITR3, 8 to 15 are ITR4 to ITR11 and 16 is ITR12, RM0481 43.6.3
 *        and 44.5.3 TIMx_SMCR. RM0481 Tables 456, 480, 502 and 514 connect the same source to the
 *        same ITR on TIM1, TIM2 to TIM5, TIM8, TIM12 and TIM15. ITR12 is USBSOF on TIM2 and TIM5
 *        and reserved on the other six. USBSOF has no model.
 */
inline constexpr auto h5_triggers = std::to_array<trigger_entry>({
    {0,  "tim1"     },
    {1,  "tim2"     },
    {2,  "tim3"     },
    {3,  "tim4"     },
    {8,  "tim5"     },
    {9,  "tim8"     },
    {10, "tim12"    },
    {11, "tim13_oc1"},
    {12, "tim14_oc1"},
    {13, "tim15"    },
    {14, "tim16_oc1"},
    {15, "tim17_oc1"},
    {16, "usbsof"   },
});
/** @brief RM0481 46.8.13: TIM13 and TIM14 have no CR2. RM0481 47.8.2: TIM16 and TIM17 have no
 *         master mode. */
inline constexpr auto h5_no_master =
    std::to_array<std::string_view>({"TIM13", "TIM14", "TIM16", "TIM17"});
/** @brief STM32H5 timers. */
inline constexpr tim_variant h5_tim{
    .triggers             = h5_triggers,
    .wide_master          = wide_master_timers,
    .no_master            = h5_no_master,
    .uif_overwrites_count = true};

/**
 * @brief RM0038 Table 80, TIMx internal trigger connection, on Cat.1 and Cat.2: ITR0 to ITR3 by
 *        TS 000 to 011 per slave timer. Cat.1 and Cat.2 have no TIM2_OR or TIM3_OR, RM0038
 *        17.4.19 and 17.4.20, so TIM2 ITR1 is TIM10 OC1 and TIM3 ITR2 is TIM11 OC1, the inputs
 *        those registers give at reset. Table 80 has no TIM5 row, so TIM5 has no entry.
 */
inline constexpr auto l1_triggers = std::to_array<trigger_entry>({
    {.select = 0, .source = "tim9",      .slave = "TIM2"},
    {.select = 1, .source = "tim10_oc1", .slave = "TIM2"},
    {.select = 2, .source = "tim3",      .slave = "TIM2"},
    {.select = 3, .source = "tim4",      .slave = "TIM2"},
    {.select = 0, .source = "tim9",      .slave = "TIM3"},
    {.select = 1, .source = "tim2",      .slave = "TIM3"},
    {.select = 2, .source = "tim11_oc1", .slave = "TIM3"},
    {.select = 3, .source = "tim4",      .slave = "TIM3"},
    {.select = 0, .source = "tim10_oc1", .slave = "TIM4"},
    {.select = 1, .source = "tim2",      .slave = "TIM4"},
    {.select = 2, .source = "tim3",      .slave = "TIM4"},
    {.select = 3, .source = "tim9",      .slave = "TIM4"},
    {.select = 0, .source = "tim2",      .slave = "TIM9"},
    {.select = 1, .source = "tim3",      .slave = "TIM9"},
    {.select = 2, .source = "tim10_oc1", .slave = "TIM9"},
    {.select = 3, .source = "tim11_oc1", .slave = "TIM9"},
});
/**
 * @brief RM0038 Table 80 on Cat.3 to Cat.6: TIM2_OR.ITR1_RMP selects TIM10 OC1 (0) or TIM5 TRGO
 *        (1) for TIM2 ITR1, TIM3_OR.ITR2_RMP selects TIM11 OC1 (0) or TIM5 TRGO (1) for TIM3 ITR2,
 *        RM0038 17.4.19 and 17.4.20.
 */
inline constexpr auto l1_cat3_triggers = std::to_array<trigger_entry>({
    {.select = 0, .source = "tim9", .slave = "TIM2"},
    {.select = 1,
     .source = "tim10_oc1",
     .slave  = "TIM2",
     .remap  = trigger_remap{.reg = "OR", .field = "ITR1_RMP", .value = 0}},
    {.select = 1,
     .source = "tim5",
     .slave  = "TIM2",
     .remap  = trigger_remap{.reg = "OR", .field = "ITR1_RMP", .value = 1}},
    {.select = 2, .source = "tim3", .slave = "TIM2"},
    {.select = 3, .source = "tim4", .slave = "TIM2"},
    {.select = 0, .source = "tim9", .slave = "TIM3"},
    {.select = 1, .source = "tim2", .slave = "TIM3"},
    {.select = 2,
     .source = "tim11_oc1",
     .slave  = "TIM3",
     .remap  = trigger_remap{.reg = "OR", .field = "ITR2_RMP", .value = 0}},
    {.select = 2,
     .source = "tim5",
     .slave  = "TIM3",
     .remap  = trigger_remap{.reg = "OR", .field = "ITR2_RMP", .value = 1}},
    {.select = 3, .source = "tim4", .slave = "TIM3"},
    {.select = 0, .source = "tim10_oc1", .slave = "TIM4"},
    {.select = 1, .source = "tim2", .slave = "TIM4"},
    {.select = 2, .source = "tim3", .slave = "TIM4"},
    {.select = 3, .source = "tim9", .slave = "TIM4"},
    {.select = 0, .source = "tim2", .slave = "TIM9"},
    {.select = 1,
     .source = "tim3",
     .slave  = "TIM9",
     .remap  = trigger_remap{.reg = "OR", .field = "ITR1_RMP", .value = 0}},
    {.select = 1,
     .source = "ts_io",
     .slave  = "TIM9",
     .remap  = trigger_remap{.reg = "OR", .field = "ITR1_RMP", .value = 1}},
    {.select = 2, .source = "tim10_oc1", .slave = "TIM9"},
    {.select = 3, .source = "tim11_oc1", .slave = "TIM9"},
});
/**
 * @brief RM0038 17.3.3 and 17.4.3: TIM2 to TIM5 take TIMx_ETR from their ETR pin. The datasheet
 *        pin tables name the TIM2 pad TIM2_CH1_ETR, at AF1 on PA0, PA5, PA15 and PE9, so the
 *        `tim2_ch1` alternate carries both TI1 and ETR.
 */
inline constexpr auto l1_etr_pins = std::to_array<etr_pin>({
    {.instance = "TIM2", .signal = "tim2_ch1"},
    {.instance = "TIM3", .signal = "tim3_etr"},
    {.instance = "TIM4", .signal = "tim4_etr"},
    {.instance = "TIM5", .signal = "tim5_etr"},
});
/**
 * @brief RM0038 18.2.1, 18.2.2, 18.4.1, 18.5.1 and 19.2: TIM6, TIM7, TIM10, TIM11 and the Cat.1 and
 *        Cat.2 TIM9 count up only.
 */
inline constexpr auto l1_up_only =
    std::to_array<std::string_view>({"TIM6", "TIM7", "TIM9", "TIM10", "TIM11"});
/** @brief RM0038 18.2.1 and 19.2: on Cat.3 and up TIM9 counts up, down and centre-aligned. */
inline constexpr auto l1_cat3_up_only =
    std::to_array<std::string_view>({"TIM6", "TIM7", "TIM10", "TIM11"});
/** @brief RM0038 18.5: TIM10 and TIM11 have no CR2. */
inline constexpr auto l1_no_master = std::to_array<std::string_view>({"TIM10", "TIM11"});
/**
 * @brief RM0038 18.4.14 TIM9_OR: TI1_RMP 01 connects the LSE to tim_ti1, 00, 10 and 11 the
 *        TIM9_CH1 pin. 18.5.12 TIM10_OR: TI1_RMP 00 the TIM10_CH1 pin, 01 to 11 the LSI, the LSE
 *        and the RTC wakeup interrupt. 18.5.13 TIM11_OR: TI1_RMP 00 and 11 the TIM11_CH1 pin, 01
 *        and 10 the MSI and HSE_RTC.
 */
inline constexpr auto l1_input_remaps = std::to_array<input_remap>({
    {.slave = "TIM9",  .channel = 0, .reg = "OR", .field = "TI1_RMP", .pin_values = 0b1101},
    {.slave = "TIM10", .channel = 0, .reg = "OR", .field = "TI1_RMP", .pin_values = 0b0001},
    {.slave = "TIM11", .channel = 0, .reg = "OR", .field = "TI1_RMP", .pin_values = 0b1001},
});
/**
 * @brief The Cat.1 and Cat.2 remaps, and RM0038 18.5.12 and 18.5.13 on Cat.3 to Cat.6: TI1_RMP_RI 1
 *        connects the routing interface to tim_ti1 of TIM10 and TIM11, whatever TI1_RMP.
 */
inline constexpr auto l1_cat3_input_remaps = std::to_array<input_remap>({
    {.slave = "TIM9",  .channel = 0, .reg = "OR", .field = "TI1_RMP",    .pin_values = 0b1101},
    {.slave = "TIM10", .channel = 0, .reg = "OR", .field = "TI1_RMP",    .pin_values = 0b0001},
    {.slave = "TIM10", .channel = 0, .reg = "OR", .field = "TI1_RMP_RI", .pin_values = 0b01  },
    {.slave = "TIM11", .channel = 0, .reg = "OR", .field = "TI1_RMP",    .pin_values = 0b1001},
    {.slave = "TIM11", .channel = 0, .reg = "OR", .field = "TI1_RMP_RI", .pin_values = 0b01  },
});
/**
 * @brief STM32L1 TIM2 to TIM4, TIM6, TIM7 and TIM9 to TIM11 of Cat.1 and Cat.2, RM0038 17.4, 18.4,
 *        18.5 and 19.4: CR2.MMS[2:0], SMCR.SMS[2:0] and TS[2:0], no UIFREMAP.
 */
inline constexpr tim_variant l1_tim{
    .triggers     = l1_triggers,
    .wide_master  = {},
    .no_master    = l1_no_master,
    .slave_high   = false,
    .select_high  = false,
    .uif_remap    = false,
    .etr_pins     = l1_etr_pins,
    .up_only      = l1_up_only,
    .input_remaps = l1_input_remaps};
/**
 * @brief STM32L1 TIM2 to TIM7 and TIM9 to TIM11 of Cat.3 to Cat.6, with the TIM2_OR, TIM3_OR,
 *        TIM9_OR, TIM10_OR and TIM11_OR remaps.
 */
inline constexpr tim_variant l1_cat3_tim{
    .triggers     = l1_cat3_triggers,
    .wide_master  = {},
    .no_master    = l1_no_master,
    .slave_high   = false,
    .select_high  = false,
    .uif_remap    = false,
    .etr_pins     = l1_etr_pins,
    .up_only      = l1_cat3_up_only,
    .input_remaps = l1_cat3_input_remaps};

/**
 * @brief RM0351 Tables 207, 202 and 210, TIMx internal trigger connection: ITR0 to ITR3 by TS
 *        000 to 011 per slave timer, TIM2 to TIM5, then TIM1 and TIM8, then TIM15.
 *        TIM2_OR1.ITR1_RMP selects TIM8 TRGO (0) or OTG_FS_SOF (1) for TIM2 ITR1, RM0351
 *        31.4.22. OTG_FS_SOF has no model.
 */
inline constexpr auto l4_triggers = std::to_array<trigger_entry>({
    {.select = 0, .source = "tim1", .slave = "TIM2"},
    {.select = 1,
     .source = "tim8",
     .slave  = "TIM2",
     .remap  = trigger_remap{.reg = "OR1", .field = "ITR1_RMP", .value = 0}},
    {.select = 1,
     .source = "otg_fs_sof",
     .slave  = "TIM2",
     .remap  = trigger_remap{.reg = "OR1", .field = "ITR1_RMP", .value = 1}},
    {.select = 2, .source = "tim3", .slave = "TIM2"},
    {.select = 3, .source = "tim4", .slave = "TIM2"},
    {.select = 0, .source = "tim1", .slave = "TIM3"},
    {.select = 1, .source = "tim2", .slave = "TIM3"},
    {.select = 2, .source = "tim15", .slave = "TIM3"},
    {.select = 3, .source = "tim4", .slave = "TIM3"},
    {.select = 0, .source = "tim1", .slave = "TIM4"},
    {.select = 1, .source = "tim2", .slave = "TIM4"},
    {.select = 2, .source = "tim3", .slave = "TIM4"},
    {.select = 3, .source = "tim8", .slave = "TIM4"},
    {.select = 0, .source = "tim2", .slave = "TIM5"},
    {.select = 1, .source = "tim3", .slave = "TIM5"},
    {.select = 2, .source = "tim4", .slave = "TIM5"},
    {.select = 3, .source = "tim8", .slave = "TIM5"},
    {.select = 0, .source = "tim15", .slave = "TIM1"},
    {.select = 1, .source = "tim2", .slave = "TIM1"},
    {.select = 2, .source = "tim3", .slave = "TIM1"},
    {.select = 3, .source = "tim4", .slave = "TIM1"},
    {.select = 0, .source = "tim1", .slave = "TIM8"},
    {.select = 1, .source = "tim2", .slave = "TIM8"},
    {.select = 2, .source = "tim4", .slave = "TIM8"},
    {.select = 3, .source = "tim5", .slave = "TIM8"},
    {.select = 0, .source = "tim1", .slave = "TIM15"},
    {.select = 1, .source = "tim3", .slave = "TIM15"},
    {.select = 2, .source = "tim16_oc1", .slave = "TIM15"},
    {.select = 3, .source = "tim17_oc1", .slave = "TIM15"},
});
/**
 * @brief RM0351 30.3.4, 31.3.3 and DS10198 Tables 17 and 18: TIM1, TIM2 to TIM5 and TIM8 take ETR
 *        from a TIMx_ETR pin alternate of its own, such as TIM2_ETR on PA0 at AF14.
 */
inline constexpr auto l4_etr_pins = std::to_array<etr_pin>({
    {.instance = "TIM2", .signal = "tim2_etr"},
    {.instance = "TIM3", .signal = "tim3_etr"},
    {.instance = "TIM4", .signal = "tim4_etr"},
    {.instance = "TIM5", .signal = "tim5_etr"},
    {.instance = "TIM1", .signal = "tim1_etr"},
    {.instance = "TIM8", .signal = "tim8_etr"},
});
/**
 * @brief RM0351 Table 61: TIM6 shares NVIC line 54 with the DAC1 underrun, `TIM6_DAC_IRQn`, and
 *        the TIM1 break, update and trigger lines are shared with TIM15, TIM16 and TIM17.
 */
inline constexpr auto l4_tim_irqs = std::to_array<tim_irq>({
    {.instance = "TIM6", .irq = "TIM6_DAC_IRQn"},
    {.instance = "TIM1", .irq = "TIM1_BRK_TIM15_IRQn", .group = "BRK"},
    {.instance = "TIM1", .irq = "TIM1_UP_TIM16_IRQn", .group = "UP"},
    {.instance = "TIM1", .irq = "TIM1_TRG_COM_TIM17_IRQn", .group = "TRG_COM"},
    {.instance = "TIM15", .irq = "TIM1_BRK_TIM15_IRQn"},
    {.instance = "TIM16", .irq = "TIM1_UP_TIM16_IRQn"},
    {.instance = "TIM17", .irq = "TIM1_TRG_COM_TIM17_IRQn"},
});
/** @brief RM0351 33.3, 33.4.1 and 32.5.1: TIM6, TIM7 and TIM15 to TIM17 count up only. */
inline constexpr auto l4_up_only =
    std::to_array<std::string_view>({"TIM6", "TIM7", "TIM15", "TIM16", "TIM17"});
/** @brief RM0351 32.7.1 and 32.7.2: TIM16 and TIM17 have no master mode. */
inline constexpr auto l4_no_master = std::to_array<std::string_view>({"TIM16", "TIM17"});
/**
 * @brief RM0351 31.4.22 TIM2_OR1: TI4_RMP 00 keeps tim_ti4 on its pin, 01 to 11 give COMP1,
 *        COMP2 or their OR, ETR_RMP 0 keeps tim_etr_in on the TIM2_ETR pin, 1 gives the LSE.
 *        31.4.24 TIM2_OR2: ETRSEL 000 leaves tim_etr_in to ETR_RMP, 001 and 010 give COMP1 and
 *        COMP2, the others are reserved. 31.4.23 TIM3_OR1: TI1_RMP 00 keeps tim_ti1 on its pin,
 *        01 to 11 give COMP1, COMP2 or their OR. 31.4.25 TIM3_OR2: ETRSEL 000 keeps tim_etr_in on
 *        the TIM3_ETR pin, 001 gives COMP1, the others are reserved. 30.4.23 and 30.4.24
 *        TIM1_OR1 and TIM8_OR1: TI1_RMP 0 keeps tim_ti1 on its pin, 1 gives COMP1 on TIM1 and
 *        COMP2 on TIM8, and ETR_ADCx_RMP 01 to 11 OR an ADC analog watchdog into tim_etr_in.
 *        30.4.28 and 30.4.30 TIM1_OR2 and TIM8_OR2: ETRSEL 000 keeps tim_etr_in on the pin and
 *        the ADC watchdogs, 001 and 010 give COMP1 and COMP2, the others are reserved.
 *        32.6.19 TIM15_OR1: TI1_RMP 0 keeps tim_ti1 on its pin, 1 gives the LSE, ENCODER_MODE
 *        01 to 11 give tim_ti1 and tim_ti2 the TIM2, TIM3 or TIM4 IC1 and IC2. 32.7.17 and
 *        32.7.19 TIM16_OR1 and TIM17_OR1: TI1_RMP 00 keeps tim_ti1 on its pin, 01 to 11 give
 *        the LSI, LSE or RTC wakeup on TIM16, the MSI, HSE/32 or MCO on TIM17.
 */
inline constexpr auto l4_input_remaps = std::to_array<input_remap>({
    {.slave = "TIM2",   .channel = 3,             .reg = "OR1", .field = "TI4_RMP",      .pin_values = 0b0001},
    {.slave = "TIM2",   .channel = tim_etr_input, .reg = "OR1", .field = "ETR_RMP",      .pin_values = 1     },
    {.slave = "TIM2",   .channel = tim_etr_input, .reg = "OR2", .field = "ETRSEL",       .pin_values = 1     },
    {.slave = "TIM3",   .channel = 0,             .reg = "OR1", .field = "TI1_RMP",      .pin_values = 0b0001},
    {.slave = "TIM3",   .channel = tim_etr_input, .reg = "OR2", .field = "ETRSEL",       .pin_values = 1     },
    {.slave = "TIM1",   .channel = 0,             .reg = "OR1", .field = "TI1_RMP",      .pin_values = 0b01  },
    {.slave = "TIM1",   .channel = tim_etr_input, .reg = "OR2", .field = "ETRSEL",       .pin_values = 1     },
    {.slave   = "TIM1",
     .channel = tim_etr_input,
     .reg     = "OR1",
     .field   = "ETR_ADC1_RMP",
     .ored    = true                                                                                         },
    {.slave   = "TIM1",
     .channel = tim_etr_input,
     .reg     = "OR1",
     .field   = "ETR_ADC3_RMP",
     .ored    = true                                                                                         },
    {.slave = "TIM8",   .channel = 0,             .reg = "OR1", .field = "TI1_RMP",      .pin_values = 0b01  },
    {.slave = "TIM8",   .channel = tim_etr_input, .reg = "OR2", .field = "ETRSEL",       .pin_values = 1     },
    {.slave   = "TIM8",
     .channel = tim_etr_input,
     .reg     = "OR1",
     .field   = "ETR_ADC2_RMP",
     .ored    = true                                                                                         },
    {.slave   = "TIM8",
     .channel = tim_etr_input,
     .reg     = "OR1",
     .field   = "ETR_ADC3_RMP",
     .ored    = true                                                                                         },
    {.slave = "TIM15",  .channel = 0,             .reg = "OR1", .field = "TI1_RMP",      .pin_values = 0b01  },
    {.slave = "TIM15",  .channel = 0,             .reg = "OR1", .field = "ENCODER_MODE", .pin_values = 0b0001},
    {.slave = "TIM15",  .channel = 1,             .reg = "OR1", .field = "ENCODER_MODE", .pin_values = 0b0001},
    {.slave = "TIM16",  .channel = 0,             .reg = "OR1", .field = "TI1_RMP",      .pin_values = 0b0001},
    {.slave = "TIM17",  .channel = 0,             .reg = "OR1", .field = "TI1_RMP",      .pin_values = 0b0001},
});
/**
 * @brief STM32L4 TIM1 to TIM8 and TIM15 to TIM17, RM0351 30.4, 31.4, 32.6, 32.7 and 33.4:
 *        CR2.MMS[2:0], SMCR.SMS[3:0] with bit 3 in `SMS_1`, SMCR.TS[2:0], UIFREMAP, ETR pins,
 *        the OR1 and OR2 remaps. TIM6, TIM7 and TIM15 to TIM17 count up only, TIM6, TIM7, TIM16
 *        and TIM17 have no SMCR.
 */
inline constexpr tim_variant l4_tim{
    .triggers             = l4_triggers,
    .wide_master          = {},
    .no_master            = l4_no_master,
    .slave_high           = true,
    .select_high          = false,
    .uif_remap            = true,
    .uif_overwrites_count = true,
    .etr_pins             = l4_etr_pins,
    .up_only              = l4_up_only,
    .input_remaps         = l4_input_remaps,
    .irq_names            = l4_tim_irqs};

/**
 * @brief Factory of the TIM model, RM0456 sections 54 to 57 and RM0481 sections 43 to 48.
 * @param variant What differs between the timers of the family the row serves.
 */
[[nodiscard]] auto tim_factory(tim_variant variant) -> model_factory;

/** @brief The TIM model rows. */
[[nodiscard]] auto tim_rows() -> std::span<builtin const>;
}
