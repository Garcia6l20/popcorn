#include "adc_pads.hpp"
#include "dma.hpp"
#include "dma_signal.hpp"
#include "factories.hpp"
#include "factory_data.hpp"
#include "gpio_peers.hpp"
#include "model_base.hpp"
#include "trigger_signal.hpp"

#include <popcorn/core/clock.hpp>
#include <popcorn/periph/stm32/analog.hpp>
#include <popcorn/periph/stm32/gpio.hpp>
#include <popcorn/periph/stm32/supply.hpp>

#include <reflex/enum.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace popcorn::periph::stm32
{
analog_port::~analog_port() = default;

namespace
{
constexpr std::size_t   channels           = 20;
constexpr std::size_t   offsets            = 4;
constexpr std::uint32_t conversion_end     = 0;
constexpr std::uint32_t sampling_end       = 1;
constexpr std::uint32_t ready_end          = 2;
constexpr std::uint32_t edge_due           = 3;
constexpr std::uint32_t calibration_end    = 4;
constexpr std::uint32_t injected_edge_due  = 5;
constexpr std::size_t   injected_ranks     = 4;
constexpr std::size_t   trigger_selections = 32;
constexpr std::uint32_t exten_rising       = 1;
constexpr std::uint32_t exten_falling      = 2;
constexpr std::size_t   watchdogs          = 3;
constexpr std::uint8_t  max_filter_count   = 8;

using ready_pair  = std::optional<std::array<std::uint32_t, 2>>;
using cycle_count = std::optional<std::uint32_t>;

enum class dma_mode : std::uint8_t
{
  off      = 0,
  one_shot = 1,
  mdf      = 2,
  circular = 3,
};

enum class regular_trigger : std::uint8_t
{
  tim1_oc1   = 0,
  tim1_oc2   = 1,
  tim1_oc3   = 2,
  tim2_oc2   = 3,
  tim3_trgo  = 4,
  tim4_oc4   = 5,
  exti11     = 6,
  tim8_trgo  = 7,
  tim8_trgo2 = 8,
  tim1_trgo  = 9,
  tim1_trgo2 = 10,
  tim2_trgo  = 11,
  tim4_trgo  = 12,
  tim6_trgo  = 13,
  tim15_trgo = 14,
  tim3_oc4   = 15,
  exti15     = 16,
  lptim1_ch1 = 18,
  lptim2_ch1 = 19,
  lptim3_ch1 = 20,
  lptim4_out = 21,
};

enum class h5_regular_trigger : std::uint8_t
{
  tim1_oc1   = 0,
  tim1_oc2   = 1,
  tim1_oc3   = 2,
  tim2_oc2   = 3,
  tim3_trgo  = 4,
  tim4_oc4   = 5,
  exti11     = 6,
  tim8_trgo  = 7,
  tim8_trgo2 = 8,
  tim1_trgo  = 9,
  tim1_trgo2 = 10,
  tim2_trgo  = 11,
  tim4_trgo  = 12,
  tim6_trgo  = 13,
  tim15_trgo = 14,
  tim3_oc4   = 15,
  exti15     = 16,
  lptim1_ch1 = 18,
  lptim2_ch1 = 19,
};

enum class l4_regular_trigger : std::uint8_t
{
  tim1_oc1   = 0,
  tim1_oc2   = 1,
  tim1_oc3   = 2,
  tim2_oc2   = 3,
  tim3_trgo  = 4,
  tim4_oc4   = 5,
  exti11     = 6,
  tim8_trgo  = 7,
  tim8_trgo2 = 8,
  tim1_trgo  = 9,
  tim1_trgo2 = 10,
  tim2_trgo  = 11,
  tim4_trgo  = 12,
  tim6_trgo  = 13,
  tim15_trgo = 14,
  tim3_oc4   = 15,
};

enum class l4_injected_trigger : std::uint8_t
{
  tim1_trgo  = 0,
  tim1_oc4   = 1,
  tim2_trgo  = 2,
  tim2_oc1   = 3,
  tim3_oc4   = 4,
  tim4_trgo  = 5,
  exti15     = 6,
  tim8_oc4   = 7,
  tim1_trgo2 = 8,
  tim8_trgo  = 9,
  tim8_trgo2 = 10,
  tim3_oc3   = 11,
  tim3_trgo  = 12,
  tim3_oc1   = 13,
  tim6_trgo  = 14,
  tim15_trgo = 15,
};

using wide = unsigned __int128;

[[nodiscard]] constexpr auto
    cycles_ps(std::uint64_t units, std::uint32_t per_cycle, std::uint32_t divider, std::uint64_t hz)
        -> std::uint64_t
{
  auto const period = per_cycle * wide{hz};
  return static_cast<std::uint64_t>((wide{units} * divider * ps_per_second + period - 1u) / period);
}

[[nodiscard]] constexpr auto
    half_cycles_ps(std::uint64_t half_cycles, std::uint32_t divider, std::uint64_t hz)
        -> std::uint64_t
{
  return cycles_ps(half_cycles, 2, divider, hz);
}

template <typename Enum> [[nodiscard]] auto trigger_name(std::uint32_t selection) -> std::string
{
  template for(constexpr auto e : std::define_static_array(std::meta::enumerators_of(^^Enum)))
  {
    if(std::to_underlying([:e:]) == selection)
    {
      return std::string{std::meta::identifier_of(e)};
    }
  }
  return std::format("adc_ext_trg{}", selection);
}

template <typename Enum> [[nodiscard]] auto trigger_signal(std::uint32_t selection) -> std::string
{
  auto       name = trigger_name<Enum>(selection);
  auto const at   = name.find("_oc");
  if(name.starts_with("tim") and at != std::string::npos)
  {
    name.replace(at, 3, "_cc");
  }
  return name;
}

template <typename Enum> [[nodiscard]] auto trigger_signals() -> std::array<std::string, 32> const&
{
  static auto const signals = [] {
    std::array<std::string, 32> out;
    for(std::uint32_t selection = 0; selection < out.size(); ++selection)
    {
      out[selection] = trigger_signal<Enum>(selection);
    }
    return out;
  }();
  return signals;
}

struct adc_state;
struct l4_adc_state;

struct adc_pad
{
  std::uint8_t                channel{};
  char                        port{};
  std::uint8_t                pin{};
  gpio_port*                  gpio{};
  analog_switch_source const* switch_source{};
};

struct adc_inputs
{
  std::string_view             instance;
  std::optional<std::uint32_t> vrefint;
  std::optional<std::uint32_t> sensor;
  std::string_view             irq{};
  std::uint32_t                externals{};
  std::string_view             external_names{};
};

struct threshold_names
{
  std::string_view low_register;
  std::string_view low;
  std::string_view high_register;
  std::string_view high;
};

/**
 * @brief Register-ranked sequence: SQR1.L + 1 ranks of SQ1 to SQ16 across SQR1 to SQR4, the layout
 *        of the STM32U5 and STM32H5 ADC. A family selecting channels by a bitmask supplies its
 *        own type with the same two members.
 */
class rank_sequence
{
public:
  static constexpr std::size_t ranks = 16;

  template <typename Lookup>
  explicit rank_sequence(Lookup const& lookup) : length_{lookup("ADC_SQR1", "L")}
  {
    for(std::size_t rank = 0; rank < ranks; ++rank)
    {
      sequence_[rank] =
          lookup(std::format("ADC_SQR{}", (rank + 1) / 5 + 1), std::format("SQ{}", rank + 1));
    }
  }

  [[nodiscard]] auto sequence_length(register_file const& regs) const -> std::size_t
  {
    return get(regs, length_) + std::size_t{1};
  }

  [[nodiscard]] auto next_channel(register_file const& regs, std::size_t rank) const
      -> std::uint32_t
  {
    return get(regs, sequence_[std::min(rank, ranks - 1)]);
  }

private:
  field_ref                    length_;
  std::array<field_ref, ranks> sequence_;
};

/**
 * @brief Family traits of the STM32U5 ADC1 and ADC2, `aditf5_40v1_U5_Cube`, RM0456 section 33:
 *        register names, 14-bit codes, the sampling and SAR times in half ADC clock cycles, the
 *        EXTSEL map of Table 307, tLATR, the factory words and the eight-result FIFO.
 */
struct u5_adc
{
  using sequence      = rank_sequence;
  using trigger_enum  = regular_trigger;
  using injected_enum = void;
  using state         = adc_state;

  static constexpr std::string_view unit_name           = "ADC";
  static constexpr std::string_view config              = "ADC_CFGR1";
  static constexpr std::string_view common_unit         = "ADC12";
  static constexpr std::string_view common_register     = "ADC12_CCR";
  static constexpr std::string_view common_vrefint      = "VREFEN";
  static constexpr std::string_view common_sensor       = "VSENSESEL";
  static constexpr std::string_view common_prescaler    = "PRESC";
  static constexpr std::string_view common_clock_mode   = {};
  static constexpr std::string_view common_dual         = {};
  static constexpr std::string_view common_status       = {};
  static constexpr bool             regulator_flag      = true;
  static constexpr std::size_t      result_depth        = 8;
  static constexpr bool             overrun_flush       = false;
  static constexpr bool             dma_end_stops       = false;
  static constexpr bool             dma_end_freezes     = false;
  static constexpr bool             trigger_waits_edge  = false;
  static constexpr ready_pair       ready_cycles        = std::nullopt;
  static constexpr std::uint32_t    result_mask         = 0xFFFF'FFFF;
  static constexpr cycle_count      ready_adc_cycles    = std::nullopt;
  static constexpr cycle_count      calibration_cycles  = std::nullopt;
  static constexpr std::uint32_t    enable_lockout      = 0;
  static constexpr unsigned         bits                = 14;
  static constexpr address          ts_cal1_address     = 0x0BFA'0710;
  static constexpr address          vrefint_cal_address = 0x0BFA'07A5;
  static constexpr std::uint32_t    factory_vdda_mv     = 3000;
  static constexpr std::string_view externals           = "from 1 to 17";
  static constexpr std::size_t      channel_count       = 20;
  static constexpr bool             bulb                = true;
  static constexpr bool             sampling_trigger    = true;
  static constexpr std::string_view differential        = "DIFSEL";
  static constexpr std::string_view sampling_plus       = {};
  static constexpr std::string_view ratio_field         = "OSR";
  static constexpr std::string_view filter_register     = "ADC_HTR1";
  static constexpr std::string_view filter_field        = "AWDFILT1";
  static constexpr bool             filter_gates_dma    = true;
  static constexpr bool             preselection        = true;
  static constexpr bool             gain                = true;
  static constexpr bool             left_shift          = true;
  static constexpr bool             low_frequency       = true;
  static constexpr std::string_view align               = {};
  static constexpr std::string_view offset_enable       = {};
  static constexpr bool             subtracts_offset    = false;
  static constexpr std::string_view injected_oversample = {};
  static constexpr bool             injected_ovs        = false;
  static constexpr bool             triggered_ovs       = false;
  static constexpr std::string_view option_register     = {};
  static constexpr std::string_view option_switch       = {};
  static constexpr std::string_view option_owner        = {};
  static constexpr std::uint32_t    option_channel      = 0;
  static constexpr std::string_view cite_option         = {};
  static constexpr std::uint64_t    min_hz              = 5'000'000;
  static constexpr std::uint64_t    max_hz              = 55'000'000;
  static constexpr std::string_view clock_range         = "5 to 55 MHz";
  static constexpr std::string_view cite_control        = "RM0456 33.6.3";
  static constexpr std::string_view cite_discontinuous  = "RM0456 33.4.15";

  static constexpr std::array<std::string_view, 1> dma_fields{"DMNGT"};
  static constexpr std::array<adc_inputs, 2>       inputs{
      {{.instance = "ADC1", .vrefint = 0, .sensor = 19},
       {.instance = "ADC2", .vrefint = 0, .sensor = 19}}
  };
  static constexpr std::array<threshold_names, watchdogs> thresholds{
      {{"ADC_LTR1", "LTR1", "ADC_HTR1", "HTR1"},
       {"ADC_LTR2", "LTR2", "ADC_HTR2", "HTR2"},
       {"ADC_LTR3", "LTR3", "ADC_HTR3", "HTR3"}}
  };

  static constexpr std::array<std::uint32_t, 8>  sampling_half_cycles{10, 12,  24,  40,
                                                                      72, 136, 782, 1628};
  static constexpr std::array<std::uint32_t, 4>  sar_half_cycles{34, 30, 26, 22};
  static constexpr std::array<std::uint32_t, 12> prescalers{1,  2,  4,  6,  8,   10,
                                                            12, 16, 32, 64, 128, 256};

  [[nodiscard]] static constexpr auto external(std::uint32_t channel) -> bool
  {
    return channel >= 1 and channel <= 17;
  }

  [[nodiscard]] static constexpr auto decode_dma(std::array<std::uint32_t, 1> raw) -> dma_mode
  {
    return static_cast<dma_mode>(raw[0] & 3u);
  }

  [[nodiscard]] static constexpr auto ratio(std::uint32_t raw) -> std::uint32_t
  {
    return raw + 1u;
  }

  [[nodiscard]] static constexpr auto
      compared(std::size_t, std::uint32_t result, std::uint32_t resolution, bool) -> std::uint64_t
  {
    return std::uint64_t{result} << (2 * resolution);
  }

  [[nodiscard]] static constexpr auto trigger_latency_eighths(std::uint32_t divider, std::uint32_t)
      -> std::uint32_t
  {
    return 8u * (divider == 1 ? 3u : 3u * divider + 1u);
  }
};

/**
 * @brief Family traits of the STM32H5 ADC1 and ADC2, `aditf5_v3_0_H5_Cube`, RM0481 section 28:
 *        register names, 12-bit codes, the sampling times of RM0481 28.4.12 and the SAR times of
 *        Table 285 in half ADC clock cycles, SMPPLUS, the EXTSEL map of Table 282, tLATR and the
 *        ADC clock range of DS14121 Table 96, the factory words of DS14121 Tables 28 and 102, the
 *        ADCC common block with CKMODE, and the three-result FIFO of Table 279.
 */
struct h5_adc
{
  using sequence      = rank_sequence;
  using trigger_enum  = h5_regular_trigger;
  using injected_enum = void;
  using state         = adc_state;

  static constexpr std::string_view unit_name          = "ADC";
  static constexpr std::string_view config             = "ADC_CFGR";
  static constexpr std::string_view common_unit        = "ADCC";
  static constexpr std::string_view common_register    = "ADC_CCR";
  static constexpr std::string_view common_vrefint     = "VREFEN";
  static constexpr std::string_view common_sensor      = "TSEN";
  static constexpr std::string_view common_prescaler   = "PRESC";
  static constexpr std::string_view common_clock_mode  = "CKMODE";
  static constexpr std::string_view common_dual        = "DUAL";
  static constexpr std::string_view common_status      = "ADC_CSR";
  static constexpr std::string_view status_master      = "ADC1";
  static constexpr std::string_view status_slave       = "ADC2";
  static constexpr std::uint32_t    status_flags       = 0x7FF;
  static constexpr unsigned         status_slave_shift = 16;
  static constexpr bool             regulator_flag     = false;
  static constexpr std::size_t      result_depth       = 3;
  /** @brief An overrun under OVRMOD 0 keeps the FIFO head only, measured on an STM32H573. */
  static constexpr bool overrun_flush = true;
  /** @brief The end of a one-shot DMA transfer clears ADSTART, measured on an STM32H573. */
  static constexpr bool dma_end_stops   = true;
  static constexpr bool dma_end_freezes = false;
  /** @brief A trigger at CKMODE 00 waits for the next ADC clock edge, measured on an STM32H573. */
  static constexpr bool trigger_waits_edge = true;
  /**
   * @brief HCLK cycles from the ADEN edge to ADRDY with ADVREGEN 1 and 0, measured on an
   *        STM32H573 with the ADC clock at HCLK.
   */
  static constexpr ready_pair       ready_cycles        = std::array<std::uint32_t, 2>{35, 45};
  static constexpr cycle_count      ready_adc_cycles    = std::nullopt;
  static constexpr cycle_count      calibration_cycles  = std::nullopt;
  static constexpr std::uint32_t    enable_lockout      = 0;
  static constexpr std::uint32_t    result_mask         = 0xFFFF;
  static constexpr unsigned         bits                = 12;
  static constexpr address          ts_cal1_address     = 0x08FF'F814;
  static constexpr address          vrefint_cal_address = 0x08FF'F810;
  static constexpr std::uint32_t    factory_vdda_mv     = 3300;
  static constexpr std::string_view externals           = "0 to 15, 18 or 19";
  static constexpr std::size_t      channel_count       = 20;
  static constexpr bool             bulb                = true;
  static constexpr bool             sampling_trigger    = true;
  static constexpr std::string_view differential        = "DIFSEL";
  static constexpr std::string_view sampling_plus       = "SMPPLUS";
  static constexpr std::uint32_t    sampling_plus_half  = 7;
  static constexpr std::string_view ratio_field         = "OVSR";
  static constexpr std::string_view filter_register     = "ADC_TR1";
  static constexpr std::string_view filter_field        = "AWDFILT";
  static constexpr bool             filter_gates_dma    = false;
  static constexpr bool             preselection        = false;
  static constexpr bool             gain                = false;
  static constexpr bool             left_shift          = false;
  static constexpr bool             low_frequency       = false;
  static constexpr std::string_view align               = "ALIGN";
  static constexpr std::string_view offset_enable       = "OFFSET_EN";
  static constexpr bool             subtracts_offset    = false;
  static constexpr std::string_view injected_oversample = "JOVSE";
  static constexpr bool             injected_ovs        = false;
  static constexpr bool             triggered_ovs       = false;
  static constexpr std::string_view option_register     = "ADC_OR";
  static constexpr std::string_view option_switch       = "OP0";
  static constexpr std::string_view option_owner        = "ADC1";
  static constexpr std::uint32_t    option_channel      = 0;
  static constexpr std::string_view cite_option         = "RM0481 28.6.23";
  static constexpr std::string_view cite_clock_mode     = "RM0481 28.7.2";
  static constexpr std::uint64_t    min_hz              = 1'500'000;
  static constexpr std::uint64_t    max_hz              = 75'000'000;
  static constexpr std::string_view clock_range         = "1.5 to 75 MHz";
  static constexpr std::string_view cite_control        = "RM0481 28.4.10";
  static constexpr std::string_view cite_discontinuous  = "RM0481 28.4.20";

  static constexpr std::array<std::string_view, 2> dma_fields{"DMAEN", "DMACFG"};
  static constexpr std::array<std::uint32_t, 3>    synchronous_dividers{1, 2, 4};
  static constexpr std::array<adc_inputs, 2>       inputs{
      {{.instance = "ADC1", .vrefint = 17, .sensor = 16},
       {.instance = "ADC2", .vrefint = std::nullopt, .sensor = std::nullopt}}
  };
  static constexpr std::array<threshold_names, watchdogs> thresholds{
      {{"ADC_TR1", "LT1", "ADC_TR1", "HT1"},
       {"ADC_TR2", "LT2", "ADC_TR2", "HT2"},
       {"ADC_TR3", "LT3", "ADC_TR3", "HT3"}}
  };

  static constexpr std::array<std::uint32_t, 8>  sampling_half_cycles{5,  13,  25,  49,
                                                                      95, 185, 495, 1281};
  static constexpr std::array<std::uint32_t, 4>  sar_half_cycles{25, 21, 17, 13};
  static constexpr std::array<std::uint32_t, 12> prescalers{1,  2,  4,  6,  8,   10,
                                                            12, 16, 32, 64, 128, 256};

  [[nodiscard]] static constexpr auto external(std::uint32_t channel) -> bool
  {
    return channel < channels and channel != 16 and channel != 17;
  }

  [[nodiscard]] static constexpr auto decode_dma(std::array<std::uint32_t, 2> raw) -> dma_mode
  {
    if(raw[0] == 0)
    {
      return dma_mode::off;
    }
    return raw[1] != 0 ? dma_mode::circular : dma_mode::one_shot;
  }

  [[nodiscard]] static constexpr auto ratio(std::uint32_t raw) -> std::uint32_t
  {
    return 2u << raw;
  }

  [[nodiscard]] static constexpr auto compared(
      std::size_t   watchdog,
      std::uint32_t result,
      std::uint32_t resolution,
      bool          oversampled) -> std::uint64_t
  {
    if(oversampled)
    {
      return watchdog == 0 ? result >> 4 : result >> 8;
    }
    auto const raw = std::uint64_t{result} << (2 * resolution);
    return watchdog == 0 ? raw : raw >> 4;
  }

  [[nodiscard]] static constexpr auto left_aligned(std::uint32_t result, std::uint32_t resolution)
      -> std::uint32_t
  {
    return resolution == 3 ? result << 2 : result << (4 + 2 * resolution);
  }

  [[nodiscard]] static constexpr auto
      trigger_latency_eighths(std::uint32_t, std::uint32_t clock_mode) -> std::uint32_t
  {
    constexpr std::array<std::uint32_t, 4> eighths{36, 20, 20, 18};
    return eighths[clock_mode & 3u];
  }
};

/**
 * @brief Family traits of the STM32L47x and STM32L48x ADC1, ADC2 and ADC3, `aditf5_v2_0_Cube`,
 *        RM0351 section 18: register names, 12-bit codes, the sampling times of RM0351 18.4.12
 *        and the SAR times of Table 113 in half ADC clock cycles, the EXTSEL map of Table 111,
 *        tLATR and the ADC clock range of DS10198 Table 76, the factory words of DS10198 Tables 8
 *        and 9, the ADC_Common block with CKMODE, CH17SEL and CH18SEL, and a single ADC_DR.
 */
struct l4_adc
{
  using sequence      = rank_sequence;
  using trigger_enum  = l4_regular_trigger;
  using injected_enum = l4_injected_trigger;
  using state         = l4_adc_state;

  static constexpr std::string_view unit_name          = "ADC";
  static constexpr std::string_view config             = "ADC_CFGR";
  static constexpr std::string_view common_unit        = "ADC_Common";
  static constexpr std::string_view common_register    = "ADC_CCR";
  static constexpr std::string_view common_vrefint     = "VREFEN";
  static constexpr std::string_view common_sensor      = "CH17SEL";
  static constexpr std::string_view common_prescaler   = "PRESC";
  static constexpr std::string_view common_clock_mode  = "CKMODE";
  static constexpr std::string_view common_dual        = "DUAL";
  static constexpr std::string_view common_status      = "ADC_CSR";
  static constexpr std::string_view status_master      = "ADC1";
  static constexpr std::string_view status_slave       = "ADC2";
  static constexpr std::uint32_t    status_flags       = 0x7FF;
  static constexpr unsigned         status_slave_shift = 16;
  static constexpr bool             regulator_flag     = false;
  static constexpr std::size_t      result_depth       = 1;
  static constexpr bool             overrun_flush      = false;
  static constexpr bool             dma_end_stops      = false;
  static constexpr bool             dma_end_freezes    = true;
  /** @brief A trigger at CKMODE 00 waits for the next ADC clock edge, inferred from DS10198. */
  static constexpr bool       trigger_waits_edge = true;
  static constexpr ready_pair ready_cycles       = std::nullopt;
  /** @brief tSTAB, DS10198 Table 76 one conversion cycle, as one 12-bit conversion, inferred. */
  static constexpr cycle_count ready_adc_cycles = 15;
  /** @brief tCAL, DS10198 Table 76 typical. */
  static constexpr cycle_count calibration_cycles = 116;
  /** @brief ADC clock cycles after ADCAL clears that drop an ADEN write, RM0351 18.4.9. */
  static constexpr std::uint32_t    enable_lockout      = 4;
  static constexpr std::uint32_t    result_mask         = 0xFFFF;
  static constexpr unsigned         bits                = 12;
  static constexpr address          ts_cal1_address     = 0x1FFF'75A8;
  static constexpr address          vrefint_cal_address = 0x1FFF'75AA;
  static constexpr std::uint32_t    factory_vdda_mv     = 3000;
  static constexpr std::string_view externals           = "1 to 16";
  static constexpr std::size_t      channel_count       = 19;
  static constexpr bool             bulb                = false;
  static constexpr bool             sampling_trigger    = false;
  static constexpr std::string_view differential        = {};
  static constexpr std::string_view sampling_plus       = {};
  static constexpr std::string_view ratio_field         = "OVSR";
  static constexpr std::string_view filter_register     = {};
  static constexpr std::string_view filter_field        = {};
  static constexpr bool             filter_gates_dma    = false;
  static constexpr bool             preselection        = false;
  static constexpr bool             gain                = false;
  static constexpr bool             left_shift          = false;
  static constexpr bool             low_frequency       = false;
  static constexpr std::string_view align               = "ALIGN";
  static constexpr std::string_view offset_enable       = "OFFSET_EN";
  static constexpr bool             subtracts_offset    = true;
  static constexpr std::string_view injected_oversample = "JOVSE";
  static constexpr bool             injected_ovs        = true;
  static constexpr bool             triggered_ovs       = true;
  static constexpr std::string_view option_register     = {};
  static constexpr std::string_view option_switch       = {};
  static constexpr std::string_view option_owner        = {};
  static constexpr std::uint32_t    option_channel      = 0;
  static constexpr std::string_view cite_option         = {};
  static constexpr std::string_view cite_clock_mode     = "RM0351 18.8.2";
  static constexpr std::uint64_t    min_hz              = 140'000;
  static constexpr std::uint64_t    max_hz              = 80'000'000;
  static constexpr std::string_view clock_range         = "0.14 to 80 MHz";
  static constexpr std::string_view cite_control        = "RM0351 18.4.10";
  static constexpr std::string_view cite_discontinuous  = "RM0351 18.4.20";

  static constexpr std::array<std::string_view, 2> dma_fields{"DMAEN", "DMACFG"};
  static constexpr std::array<std::uint32_t, 3>    synchronous_dividers{1, 2, 4};
  static constexpr std::array<adc_inputs, 3>       inputs{
      {{.instance = "ADC1", .vrefint = 0, .sensor = 17, .irq = "ADC1_2_IRQn"},
       {.instance = "ADC2", .vrefint = std::nullopt, .sensor = std::nullopt, .irq = "ADC1_2_IRQn"},
       {.instance       = "ADC3",
        .vrefint        = std::nullopt,
        .sensor         = 17,
        .irq            = "ADC3_IRQn",
        .externals      = 0x3FDE,
        .external_names = "1 to 4 or 6 to 13"}}
  };
  static constexpr std::array<threshold_names, watchdogs> thresholds{
      {{"ADC_TR1", "LT1", "ADC_TR1", "HT1"},
       {"ADC_TR2", "LT2", "ADC_TR2", "HT2"},
       {"ADC_TR3", "LT3", "ADC_TR3", "HT3"}}
  };

  static constexpr std::array<std::uint32_t, 8>  sampling_half_cycles{5,  13,  25,  49,
                                                                      95, 185, 495, 1281};
  static constexpr std::array<std::uint32_t, 4>  sar_half_cycles{25, 21, 17, 13};
  static constexpr std::array<std::uint32_t, 12> prescalers{1,  2,  4,  6,  8,   10,
                                                            12, 16, 32, 64, 128, 256};

  [[nodiscard]] static constexpr auto external(std::uint32_t channel) -> bool
  {
    return channel >= 1 and channel <= 16;
  }

  [[nodiscard]] static constexpr auto decode_dma(std::array<std::uint32_t, 2> raw) -> dma_mode
  {
    if(raw[0] == 0)
    {
      return dma_mode::off;
    }
    return raw[1] != 0 ? dma_mode::circular : dma_mode::one_shot;
  }

  [[nodiscard]] static constexpr auto ratio(std::uint32_t raw) -> std::uint32_t
  {
    return 2u << raw;
  }

  [[nodiscard]] static constexpr auto compared(
      std::size_t   watchdog,
      std::uint32_t result,
      std::uint32_t resolution,
      bool          oversampled) -> std::uint64_t
  {
    if(oversampled)
    {
      return watchdog == 0 ? result >> 4 : result >> 8;
    }
    auto const raw = std::uint64_t{result} << (2 * resolution);
    return watchdog == 0 ? raw : raw >> 4;
  }

  [[nodiscard]] static constexpr auto left_aligned(std::uint32_t result, std::uint32_t resolution)
      -> std::uint32_t
  {
    return resolution == 3 ? result << 2 : result << (4 + 2 * resolution);
  }

  [[nodiscard]] static constexpr auto
      trigger_latency_eighths(std::uint32_t, std::uint32_t clock_mode) -> std::uint32_t
  {
    constexpr std::array<std::uint32_t, 4> eighths{12, 16, 18, 17};
    return eighths[clock_mode & 3u];
  }

  [[nodiscard]] static constexpr auto injected_abort_latency_eighths(std::uint32_t clock_mode)
      -> std::uint32_t
  {
    constexpr std::array<std::uint32_t, 4> eighths{20, 24, 26, 25};
    return eighths[clock_mode & 3u];
  }

  [[nodiscard]] static constexpr auto
      offset_result(std::uint32_t result, std::uint32_t offset, std::uint32_t resolution, bool left)
          -> std::uint32_t
  {
    auto const shift = 2 * resolution;
    auto const signed_result =
        (static_cast<std::int32_t>(result << shift) - static_cast<std::int32_t>(offset)) >> shift;
    auto const lift = left ? (resolution == 3 ? 1u : 3u + shift) : 0u;
    return static_cast<std::uint32_t>(signed_result) << lift & result_mask;
  }
};

struct adc_edge
{
  bool          pending{};
  bool          falling{};
  bool          reposted{};
  std::uint8_t  selection{};
  std::uint32_t generation{};
  std::uint64_t ps{};

  friend auto operator==(adc_edge const&, adc_edge const&) -> bool = default;
};

struct adc_state
{
  std::uint32_t              generation{};
  std::uint32_t              ready_generation{};
  std::uint64_t              start_ps{};
  std::uint64_t              clock_hz{};
  std::uint32_t              divider{};
  std::uint32_t              held{};
  std::uint32_t              sum{};
  bool                       converting{};
  bool                       waiting_clock{};
  std::uint8_t               rank{};
  std::uint8_t               subgroup_left{};
  std::vector<std::uint32_t> data;
  bool                       dma_stopped{};
  std::uint8_t               filter_count{};
  std::array<adc_edge, 2>    edges{};
  std::uint32_t              edge_generation{};

  friend auto operator==(adc_state const&, adc_state const&) -> bool = default;
};

struct l4_adc_state
{
  std::uint32_t                generation{};
  std::uint32_t                ready_generation{};
  std::uint64_t                start_ps{};
  std::uint64_t                clock_hz{};
  std::uint32_t                divider{};
  std::uint32_t                held{};
  std::uint32_t                sum{};
  bool                         converting{};
  bool                         waiting_clock{};
  std::uint8_t                 rank{};
  std::uint8_t                 subgroup_left{};
  std::vector<std::uint32_t>   data;
  bool                         dma_stopped{};
  std::uint8_t                 filter_count{};
  std::array<adc_edge, 2>      edges{};
  std::uint32_t                edge_generation{};
  std::uint32_t                calibration_generation{};
  bool                         calibration_waiting{};
  std::uint64_t                enable_lock_ps{};
  bool                         injecting{};
  std::uint8_t                 injected_rank{};
  bool                         regular_resume{};
  bool                         regular_pending{};
  bool                         queue_pending{};
  bool                         queue_empty{true};
  std::uint8_t                 queue_count{};
  std::array<std::uint32_t, 2> queue_next{};
  std::array<adc_edge, 2>      injected_edges{};
  std::uint32_t                injected_edge_generation{};
  std::uint32_t                last_code{};
  bool                         continued{};
  std::uint32_t                kept_held{};
  std::uint32_t                kept_sum{};

  friend auto operator==(l4_adc_state const&, l4_adc_state const&) -> bool = default;
};

consteval auto adc_state_is_prefix() -> bool
{
  auto const context = std::meta::access_context::unchecked();
  auto const common  = std::meta::nonstatic_data_members_of(^^adc_state, context);
  auto const flat    = std::meta::nonstatic_data_members_of(^^l4_adc_state, context);
  if(flat.size() < common.size())
  {
    return false;
  }
  for(std::size_t index = 0; index < common.size(); ++index)
  {
    if(std::meta::identifier_of(common[index]) != std::meta::identifier_of(flat[index])
       or std::meta::type_of(common[index]) != std::meta::type_of(flat[index]))
    {
      return false;
    }
  }
  return true;
}

static_assert(adc_state_is_prefix());

struct adc_common_state
{
  friend auto operator==(adc_common_state const&, adc_common_state const&) -> bool = default;
};

template <typename Traits> [[nodiscard]] auto wiring(std::string const& instance) -> adc_inputs
{
  auto const found =
      std::ranges::find(Traits::inputs, std::string_view{instance}, &adc_inputs::instance);
  if(found == Traits::inputs.end())
  {
    throw config_error(
        instance, 0, "name",
        std::format("an ADC instance the {} model wires", Traits::common_unit));
  }
  return *found;
}

template <typename Traits>
[[nodiscard]] constexpr auto code_of(std::uint64_t microvolts, std::uint32_t vdda_mv)
    -> std::uint32_t
{
  constexpr std::uint32_t full_scale = 1u << Traits::bits;
  return static_cast<std::uint32_t>(std::min<std::uint64_t>(
      full_scale - 1u, microvolts * full_scale / (std::uint64_t{vdda_mv} * 1000u)));
}

template <typename Traits>
[[nodiscard]] constexpr auto at_vdda(std::uint32_t factory_code, std::uint32_t vdda_mv)
    -> std::uint32_t
{
  constexpr std::uint32_t full_scale = 1u << Traits::bits;
  return std::min<std::uint32_t>(full_scale - 1u, factory_code * Traits::factory_vdda_mv / vdda_mv);
}

/**
 * @brief ADC12 common registers of the STM32U5, RM0456 section 33.7: ADC12_CCR connects
 *        VREFINT with VREFEN and the temperature sensor with VSENSESEL to the ADC1 internal
 *        channels, and divides the ADC kernel clock by PRESC. Storage otherwise.
 *
 *        ADCC common registers of the STM32H5, RM0481 section 28.7: ADC_CCR connects VREFINT
 *        with VREFEN and the temperature sensor with TSEN to the ADC1 internal channels. CKMODE
 *        00 clocks ADC1 and ADC2 from their kernel clock divided by PRESC, CKMODE 01, 10 and 11
 *        from HCLK divided by 1, 2 and 4, whatever ADC12EN, RM0481 28.4.3. A DUAL write other
 *        than 0 logs a warning once, a model_error under strict_peripherals: the dual modes are
 *        not modelled and ADC1 and ADC2 convert independently. A read of ADC_CSR gives the
 *        ADC_ISR bits 10:0 of ADC1 at bits 10:0 and of ADC2 at bits 26:16, RM0481 28.7.1, and
 *        writes leave it. ADC_CDR is storage.
 *
 *        ADC_Common of the STM32L47x and STM32L48x, RM0351 section 18.8, serves ADC1, ADC2 and
 *        ADC3 as ADCC does ADC1 and ADC2: CKMODE and PRESC per RM0351 18.8.2, VREFEN connects
 *        VREFINT, CH17SEL the temperature sensor, CH18SEL VBAT, and ADC_CSR reports ADC1 as the
 *        master and ADC2 as the slave, RM0351 18.8.1. ADC3 has no ADC_CSR bits.
 */
template <typename Traits> class adc_model;

template <typename Traits>
class adc_common_model final : public dispatch_model<adc_common_model<Traits>, adc_common_state>,
                               public bus_reset_listener
{
  using base = dispatch_model<adc_common_model<Traits>, adc_common_state>;
  using base::claims;
  using base::ctx_;
  using base::desc_;
  using base::describe;
  using base::field;
  using base::reg;
  using base::warn;

public:
  adc_common_model(model_context& ctx, peripheral_desc const& desc);

  void reset(register_file& regs, reset_kind kind) override;
  void on_bus_reset() override;
  void on_event(std::uint32_t, std::uint64_t) override
  {}

  auto on_read(register_file& regs, std::uint32_t offset, std::uint8_t size, access_ctx ctx)
      -> std::expected<std::uint32_t, bus_error> override;

  void ccr_written(std::uint32_t, std::uint32_t now);
  void status_written(std::uint32_t, std::uint32_t)
  {}

  [[nodiscard]] auto clock_mode() const -> std::uint32_t
  {
    return clock_mode_ ? get(regs_, *clock_mode_) : 0u;
  }

  [[nodiscard]] auto vrefint_connected() const -> bool
  {
    return get(regs_, vrefint_) != 0;
  }

  [[nodiscard]] auto sensor_connected() const -> bool
  {
    return get(regs_, sensor_) != 0;
  }

  [[nodiscard]] auto divider() -> std::uint32_t
  {
    auto const code = get(regs_, prescaler_);
    if(code >= Traits::prescalers.size())
    {
      if(not std::exchange(reserved_logged_, true))
      {
        warn(
            Traits::common_register,
            std::format("PRESC {:#06b}: a reserved prescaler not modelled, divided by 256", code));
      }
      if(ctx_.strict_peripherals)
      {
        throw model_error(
            desc_.name, std::string{Traits::common_register}, reg(Traits::common_register).offset);
      }
    }
    return Traits::prescalers[std::min<std::size_t>(code, Traits::prescalers.size() - 1)];
  }

private:
  void                     restore_reset_values();
  [[nodiscard]] auto       status() const -> std::uint32_t;
  register_file&           regs_;
  std::optional<reg_ref>   status_;
  field_ref                vrefint_;
  field_ref                sensor_;
  field_ref                prescaler_;
  std::optional<field_ref> clock_mode_;
  std::optional<field_ref> dual_;
  bool                     reserved_logged_{};
  bool                     dual_logged_{};
};

template <typename Traits>
constexpr auto adc_common_writes = [] {
  using entry = write_entry<adc_common_model<Traits>>;
  if constexpr(Traits::common_status.empty())
  {
    return std::array<entry, 1>{
        {{Traits::common_register, &adc_common_model<Traits>::ccr_written}}};
  }
  else
  {
    return std::array<entry, 2>{
        {{Traits::common_register, &adc_common_model<Traits>::ccr_written},
         {Traits::common_status, &adc_common_model<Traits>::status_written}}
    };
  }
}();

template <typename Traits>
adc_common_model<Traits>::adc_common_model(model_context& ctx, peripheral_desc const& desc)
    : base{ctx, desc, Traits::common_unit, adc_common_writes<Traits>}, regs_{ctx.regs},
      vrefint_{field(Traits::common_register, Traits::common_vrefint)},
      sensor_{field(Traits::common_register, Traits::common_sensor)},
      prescaler_{field(Traits::common_register, Traits::common_prescaler)}
{
  if constexpr(not Traits::common_clock_mode.empty())
  {
    clock_mode_ = field(Traits::common_register, Traits::common_clock_mode);
  }
  if constexpr(not Traits::common_dual.empty())
  {
    dual_ = field(Traits::common_register, Traits::common_dual);
  }
  if constexpr(not Traits::common_status.empty())
  {
    status_ = reg(Traits::common_status);
  }
}

template <typename Traits>
void adc_common_model<Traits>::ccr_written(std::uint32_t, std::uint32_t now)
{
  if(not dual_ or dual_->of(now) == 0)
  {
    return;
  }
  if(not std::exchange(dual_logged_, true))
  {
    warn(
        Traits::common_register,
        std::format("DUAL {:#07b}: dual ADC modes not modelled", dual_->of(now)));
  }
  if(ctx_.strict_peripherals)
  {
    throw model_error(
        desc_.name, std::string{Traits::common_register}, reg(Traits::common_register).offset);
  }
}

template <typename Traits> void adc_common_model<Traits>::reset(register_file&, reset_kind)
{
  restore_reset_values();
}

template <typename Traits> void adc_common_model<Traits>::on_bus_reset()
{
  restore_reset_values();
}

template <typename Traits> void adc_common_model<Traits>::restore_reset_values()
{
  for(auto const& name : claims())
  {
    regs_.poke(reg(name).offset, describe(name).reset);
  }
}

struct offset_fields
{
  std::string_view         reg;
  field_ref                value;
  field_ref                channel;
  std::optional<field_ref> enable;
};

struct injected_fields
{
  field_ref                             stop;
  field_ref                             length;
  field_ref                             trigger;
  field_ref                             select;
  field_ref                             discontinuous;
  field_ref                             queue_disable;
  field_ref                             queue_mode;
  field_ref                             queue_overflow;
  field_ref                             automatic;
  field_ref                             watchdog1;
  field_ref                             resumed_oversampling;
  field_ref                             end_of_conversion;
  field_ref                             end_of_sequence;
  reg_ref                               sequence_register;
  std::array<field_ref, injected_ranks> sequence;
  std::array<field_ref, injected_ranks> data;
};

struct adc_clock
{
  std::uint64_t hz;
  std::uint32_t divider;
};

/**
 * @brief ADC1 of the STM32U5 and ADC2 of the STM32U59x, U5Ax, U5Fx and U5Gx, RM0456 section 33:
 *        its power-up handshake, done at the write, and regular sequences converted in virtual
 *        time. ADC2 has the internal channels of ADC1, RM0456 Figure 224, and its `adc2` kernel
 *        clock from RCC_CCIPR3.ADCDACSEL. ADVREGEN out of deep power-down sets
 *        LDORDY, DEEPPWD clears ADVREGEN, ADCAL clears at once, ADEN sets ADRDY, ADDIS clears
 *        ADEN. With EXTEN 0, an ADC_CR write taking ADSTART from 0 to 1 with ADEN starts the
 *        regular sequence, the SQR1.L + 1 ranks of SQ1 to SQ16. Each rank is one conversion of
 *        N x (SMP + TSAR) ADC clock cycles, SMP from the SMPR field of its channel, the ADC clock
 *        being the `adc1` kernel clock divided by ADC12_CCR.PRESC, N the oversampling ratio with
 *        ROVSE and 1 without. A rank starts when the one before ends: sampling never overlaps the
 *        previous conversion. EOSMP sets at the end of each sampling phase, EOC with each result,
 *        EOS with the last rank's. CONT 0 stops after the last rank and clears ADSTART, CONT 1
 *        starts over at rank 0. DISCEN 1 converts DISCNUM + 1 ranks per start, fewer in the last
 *        subgroup, the next start going on from the next rank, clears ADSTART at the EOC ending
 *        the subgroup and ignores CONT.
 *
 *        Hardware triggers, RM0456 33.4.19: with EXTEN not 0, the ADSTART write starts nothing
 *        and ADC1 listens, as a trigger_sink, to the signal EXTSEL selects in RM0456 Table 307,
 *        such as `tim15_trgo` for 14, while ADEN and ADSTART are set. EXTEN 01 detects the
 *        signal's rising edges, 10 its falling edges, 11 both: a timer's TRGO pulse rises at its
 *        event and falls one timer kernel clock later. Each detected edge while no conversion
 *        runs starts the sequence, or the next DISCEN subgroup, tLATR later: 3, 7 or 13 ADC
 *        clock cycles for PRESC 0, 1 or 2, datasheet DS13737 table 103. The datasheet gives no
 *        value for a larger PRESC: a divider D of 4 or more takes 3 x D + 1 cycles, the rule its
 *        values for D 2 and 4 follow, unverified. An edge during a conversion or the latency is
 *        ignored. An edge its source gives ahead of the machine time, the falling edge of a
 *        pulse, is taken at its own time if ADC1 still selects and detects it then, after the
 *        ADC's own events due at that time, and a reset drops it. Two such edges are held: a
 *        third one ahead, three pulses inside one timer clock, is decided against the ADC state
 *        at its arrival, so its conversion starts at its own time and the held edges then find
 *        the ADC busy.
 *        ADSTART stays set at the end of a sequence or subgroup, RM0456
 *        33.4.16, and clears with ADSTP or ADDIS only. ADC1 tells the selected signal's source
 *        when its listening starts or stops. The `timx_ocy` selections of RM0456 Table 307 and
 *        RM0481 Table 282 take the timer's `timx_ccy`, its capture/compare event pulse, as the LL
 *        driver names them "channel y event (capture compare)", inferred. The EXTI lines 11 and
 *        15 have no source on these families. A selection no model produces, the reserved EXTSEL
 *        17 and 22 to 31 included, logs a warning once per selection when ADC1 starts listening
 *        to it, a model_error under strict_peripherals, and no conversion starts. A
 *        config_error or model_error the per-sequence checks raise at a trigger surfaces from
 *        the source's event or register access that gave the edge.
 *
 *        ADSTP and ADDIS drop a conversion in progress and reset the sequence to rank 0.
 *        Without a kernel clock a conversion waits for one, logged once. Each ADEN rising edge
 *        with the ADC clock outside 5 to 55 MHz logs a warning and the conversions keep their
 *        computed time.
 *
 *        Results, RM0456 33.4.26: with OVRMOD 0, ADC_DR shows the head of an eight-result FIFO,
 *        a CPU or DMA read of ADC_DR pops it, and EOC stays set while results are left. A result
 *        finding the FIFO full sets OVR and is discarded, as is every result while OVR stays set.
 *        With OVRMOD 1, a result overwrites ADC_DR and sets OVR when EOC was still set. A
 *        debugger read pops nothing. DMNGT 01 and 11 hold the `adc1_dma` request high while a
 *        result is unread and OVR is clear. Under DMNGT 01, once the GPDMA channel serving that
 *        request completes, the conversion in progress is dropped, the sequence resets to rank
 *        0, ADSTART stays set, and no request rises until an ADC_CR write takes ADSTART from 0
 *        to 1: RM0456 ties the re-arm to nothing, this reading is unverified. A hardware trigger
 *        then starts sequences without a request, and their results land in ADC_DR: the data
 *        register RM0456 33.4.26 calls frozen after the DMA end is not modelled. DMNGT 10, the MDF
 *        path, requests nothing. ADC1_IRQn is high while an ADC_ISR flag is set with its ADC_IER
 *        enable.
 *
 *        A description with `channel_pins` holds a positive pad for every external channel, or
 *        construction throws config_error naming the channel. Every shipped description
 *        carries the table, and a hand-written one without it passes unchecked. An external
 *        channel's input is the analog level of its positive pad, gpio_port::analog_level(), so
 *        channels of several ADCs on one pad read one level. `analog_port` drive and input act
 *        on the pad's board slot. Without a pad, drive and input throw config_error naming
 *        `channel_pins.<c>` and the channel converts 0. A slot change during a conversion
 *        reaches the ADC through gpio_port::on_analog_changing() before it applies, so the
 *        samples held before it keep the old level.
 *
 *        Codes against VDDA, the board's `supply_options`: an external channel 1 to 17 reads
 *        its input, `analog_port` bank A, as V x 16384 / VDDA, floored and saturated. A
 *        bank B channel of the port is a config_error, the ADC has one bank. VREFINT
 *        reads the factory VREFINT_CAL and the temperature sensor TS_CAL1, at 30 C, both scaled
 *        by 3000 / VDDA, when ADC12_CCR connects them, and 0 otherwise. Each word is read from
 *        the description's signature content when a sequence converting its channel starts, a
 *        config_error when absent. VBAT reads 0. A 14-bit code loses two bits per RES step. The
 *        oversampler sums N codes at the resolution and shifts the sum right by OVSS, rounding to
 *        nearest. Sample k of a conversion holds the input at the end of its sampling window,
 *        SMP cycles after k x (SMP + TSAR) cycles from the start, so a drive during a conversion
 *        reaches the samples held after it. The ADC clock a conversion started with times all
 *        its samples.
 *
 *        Injected conversions are not modelled and log a warning once. DMNGT 10, AUTDLY, LFTRIG,
 *        LSHIFT, offsets, gain compensation, BULB, SMPTRIG, TROVS and the DIFSEL bit of a converted
 *        channel leave the conversions unchanged and log a warning once, a model_error under
 *        strict_peripherals. Two settings RM0456 forbids log a warning once, a model_error under
 *        strict_peripherals: an ADSTART write with ADEN 0 or ADDIS 1, ignored with ADSTART left at
 *        0, and DISCEN with CONT, converted as DISCEN alone. A sequence converting a channel whose
 *        ADC_PCSEL bit is clear logs a warning once, a model_error under strict_peripherals, and
 *        converts it as if the bit were set, RM0456 33.4.12. Only the single DMA request is
 *        modelled: the burst request RM0456 33.4.26 raises at four results is not. A GPDMA channel
 *        reading ADC_DR in bursts of more than one beat starts each burst on the single request,
 *        and a beat finding the FIFO empty reads the last result again. A reserved ADC12_CCR.PRESC
 *        divides by 256, logged once by ADC12, a model_error under strict_peripherals.
 *
 *        Analog watchdogs, RM0456 33.4.29: each regular conversion's result, after the
 *        oversampler, is compared before alignment, as Q = result << (2 x RES), the 14-bit frame
 *        of RM0456 Table 318. A guarded result is out of window when Q < LTRx or Q > HTRx,
 *        strictly. AWD1 guards the AWD1CH channel with AWD1SGL, every channel without, while
 *        AWD1EN is set. AWD2 and AWD3 guard the channels set in AWD2CH and AWD3CH. An
 *        out-of-window result sets AWDx at its EOC, before the result lands, so a result an
 *        overrun discards still sets it. AWD1 counts consecutive out-of-window results of its
 *        guarded channels, saturating at 8, and sets AWD1 at each result once the count exceeds
 *        AWDFILT1: AWDFILT1 n flags from the n + 1-th on. An in-window result restarts the count.
 *        A changed input alone sets nothing. JAWD1EN guards the injected group, which never
 *        converts. AWDFILT1 with AWD1EN and DMNGT set gates the DMA requests by the window on
 *        hardware: not modelled, logged once at the sequence start, a model_error under
 *        strict_peripherals.
 *
 *        ADC1 and ADC2 of the STM32H5, RM0481 section 28, run the same sequencer, triggers, DMA
 *        requests, oversampler and watchdogs with these differences. Codes have 12 bits, V x 4096 /
 *        VDDA, RES 0 to 3 for 12, 10, 8 and 6 bits. A conversion is N x (SMP + TSAR) ADC clock
 *        cycles, SMP 2.5, 6.5, 12.5, 24.5, 47.5, 92.5, 247.5 or 640.5 cycles, RM0481 28.4.12, SMP
 *        000 3.5 cycles with SMPR1.SMPPLUS, TSAR 12.5, 10.5, 8.5 and 6.5 cycles, Table 285. The ADC
 *        clock comes from ADCC.CKMODE. A reserved ADC_CCR.PRESC divides by 256, logged once by
 *        ADCC, a model_error under strict_peripherals. No LDORDY: with DEEPPWD clear, ADRDY sets 35
 *        HCLK cycles after the ADEN edge with ADVREGEN set and 45 with it clear, as measured on an
 *        STM32H573 with the ADC clock at HCLK, RM0481 28.4.6 and 28.4.9. ADDIS and a reset drop a
 *        pending ADRDY. The external channels are 0 to 15, 18 and 19. ADC1 reads the temperature
 *        sensor on channel 16 and VREFINT on 17, the factory TS_CAL1 and VREFINT_CAL scaled by 3300
 *        / VDDA, when ADCC connects them, Table 282. ADC2 channels 16 and 17, VBAT / 4 and VDDCORE,
 *        read 0. ADC_DR shows the head of a three-result FIFO under OVRMOD 0, Table 279 and
 *        28.4.26. A result that finds the FIFO full sets OVR and leaves the head only. While OVR
 *        stays set, a result lands when the FIFO is empty and is discarded otherwise, as measured
 *        on an STM32H573. DMAEN 1 holds the `adc<n>_dma` request, DMACFG 0 one-shot and DMACFG 1
 *        circular, as DMNGT 01 and 11 do. The end of a one-shot transfer clears ADSTART and stops
 *        the conversions, as measured on an STM32H573. The oversampling ratio is 2 to the OVSR + 1,
 *        the result is truncated to 16 bits after the shift, RM0481 28.4.29. ALIGN 1 left-aligns
 *        ADC_DR, a 6-bit code on the byte, RM0481 28.4.26, while ROVSE and JOVSE are clear. The
 *        watchdogs compare the 12-bit raw code before alignment, AWD1 with LT1 and HT1, AWD2 and
 *        AWD3 its 8 MSBs with LTx and HTx, RM0481 Tables 288 and 289. With ROVSE they compare
 *        ADC_DR[15:4] and ADC_DR[15:8]. AWDFILT in ADC_TR1 counts as AWDFILT1 does. An enabled
 *        offset, OFFSET_EN with OFFSET_CH matching and ROVSE and JOVSE clear, logs once as
 *        unmodelled. The EXTSEL map is RM0481 Table 282, 17 and 20 to 31 reserved. A sequence
 *        converting channel 0 of ADC1 or ADC2 while ADC1 ADC_OR.OP0 is clear logs a warning once, a
 *        model_error under strict_peripherals, and converts it as 0, RM0481 28.6.23. Deviation: an
 *        STM32H573 reads an internal level there. ADC2 ADC_OR is storage. At CKMODE 00 a trigger
 *        waits for the next ADC clock edge, counted from virtual time 0, and tLATR is 4.5 ADC clock
 *        cycles from it at every prescaler, as measured on an STM32H573 at /1, /2, /128 and /256.
 *        At CKMODE 01, 10 and 11 tLATR is 2.5, 2.5 and 2.25 cycles, DS14121 Table 96. Each ADEN
 *        edge with the ADC clock outside 1.5 to 75 MHz logs a warning. An ADEN edge under CKMODE 01
 *        with HCLK not SYSCLK, forbidden by RM0481 28.7.2, logs a warning once, a model_error under
 *        strict_peripherals, and converts at HCLK.
 *
 *        ADC1, ADC2 and ADC3 of the STM32L47x and STM32L48x, RM0351 section 18, run the H5 path
 *        with these differences. ADC_DR holds one result, RM0351 18.4.26: with OVRMOD 0 a result
 *        finding EOC set sets OVR and is discarded, and every result while OVR stays set. ADC1 and
 *        ADC2 raise ADC1_2_IRQn, ADC3 ADC3_IRQn. The kernel clock is the RCC_CCIPR.ADCSEL clock,
 *        common to the three ADCs. With ADVREGEN set and DEEPPWD clear, ADRDY sets tSTAB after the
 *        ADEN write, DS10198 Table 76 one conversion cycle, here 15 ADC clock cycles, a 12-bit
 *        conversion at SMP 000, inferred. The regulator start-up wait tADCVREG_STUP is the
 *        firmware's. An ADCAL write with ADEN 0, DEEPPWD 0 and ADVREGEN 1 starts a calibration:
 *        ADCAL stays set for tCAL, 116 ADC clock cycles, DS10198 Table 76, and clears, and
 *        CALFACT_S, or CALFACT_D with ADCALDIF, reads 0 from the write on, the factor of a
 *        converter with no offset, inferred. An ADCAL write with ADEN 1, DEEPPWD 1 or ADVREGEN 0 is
 *        ignored, logged once, a model_error under strict_peripherals, RM0351 18.4.8. An ADEN write
 *        while ADCAL is set or within 4 ADC clock cycles after it clears is dropped, ADEN reads 0
 *        and no ADRDY follows, RM0351 18.4.9 as corrected by ES0250 2.8.4, logged once, a
 *        model_error under strict_peripherals. Without a kernel clock a calibration waits for one.
 *        An external channel converts its input only while the GPIOx_ASCR switch of its pad is
 *        closed, RM0351 18.4.11, the pads of RM0351 Figures 68 to 70, PC0 to PC3 for channels 1 to
 *        4, PA0 to PA7 for 5 to 12, PC4, PC5, PB0, PB1 for 13 to 16 on ADC1 and ADC2, PF3 to PF10
 *        for 6 to 13 on ADC3. A sequence converting a channel with its switch open logs a warning
 *        once, a model_error under strict_peripherals, and converts it as 0, the open-switch value
 *        inferred. The external channels are 1 to 16 on ADC1 and ADC2, 1 to 4 and 6 to 13 on ADC3,
 *        RM0351 Figures 68 to 70. ADC1 reads VREFINT on channel 0 with VREFEN, ADC1 and ADC3 the
 *        temperature sensor on 17 with CH17SEL, the factory VREFINT_CAL and TS_CAL1 scaled by 3000
 *        / VDDA, RM0351 18.4.11. VBAT / 3 on 18, the DAC1 channels 17 and 18 of ADC2 and 14 and 15
 *        of ADC3, and the reserved ADC3 channels 5 and 16 read 0. Channel 19 does not exist: SQ 19
 *        converts 0 with the SMP 000 time. The EXTSEL map is RM0351 Table 111, 4 bits, the H5 codes
 *        0 to 15: EXTSEL 6 takes the `exti11` line input of the EXTI, a pin edge on the line
 *        SYSCFG_EXTICR3 selects, and the TRGO2 selections 8 and 10, which no L4 timer model
 *        produces, log a warning once, a model_error under strict_peripherals. At CKMODE 00 a
 *        trigger waits for the next ADC clock edge, counted from virtual time 0, then tLATR 1.5
 *        cycles, which spans the 1.5 to 2.5 cycles DS10198 Table 76 gives, inferred. At CKMODE 01,
 *        10 and 11 tLATR is 2, 2.25 and 2.125 cycles, DS10198 Table 76. The end of a one-shot DMA
 *        transfer leaves ADSTART set, which RM0351 18.4.15 does not list among its hardware clears,
 *        drops the conversion in progress as on the U5 and freezes ADC_DR, RM0351 18.4.26. A result
 *        while frozen sets no OVR, RM0351 18.4.26 "This avoids generating an overrun error".
 *        It leaves EOC unchanged, sets EOS at the end of the sequence and still reaches the
 *        watchdogs, inferred. ADSTART going from 0 to 1 re-arms the request and ends the freeze,
 *        inferred. Each ADEN edge with the ADC clock outside 0.14 to 80 MHz logs a warning. With
 *        ROVSE AWD1 compares ADC_DR[15:4], RM0351 18.4.30, and AWD2 and AWD3 ADC_DR[15:8] as on the
 *        H5, inferred. AWDFILT, BULB, SMPTRIG and SMPPLUS do not exist. An ADC_OFRy with OFFSET_EN
 *        set subtracts OFFSET from the code of its OFFSET_CH channel, the code left-aligned on 12
 *        bits as RM0351 Table 114 shows, the lowest enabled y first, inferred from RM0351 18.7.17,
 *        whose lowest-x rule would otherwise let a disabled ADC_OFR1 at its reset channel 0 block
 *        the others, for ADC_DR and ADC_JDRy. The result is signed, sign-extended to 16 bits when
 *        right-aligned, Figure 97, and one bit below the unsigned position with the sign at bit 15
 *        when left-aligned, Figure 99. OFFSET bits below the resolution, which Table 114 requires
 *        clear, are subtracted too and the result keeps its upper bits, inferred. ROVSE and JOVSE
 *        ignore OFFSET_EN, RM0351 18.4.26. The watchdogs compare the raw code, RM0351 18.4.29.
 *
 *        The L4 converts the injected group with JQDIS 1, RM0351 18.4.19. A JADSTART write with
 *        ADEN converts the JL + 1 channels of JSQ1 to JSQ4 into ADC_JDR1 to ADC_JDR4 at once with
 *        JEXTEN 0. With JEXTEN set, JADSTART stays set and each JEXTEN edge of the JEXTSEL event
 *        of RM0351 Table 112 converts the group after the tLATR of a regular trigger. Each
 *        conversion takes the regular time of its channel. JEOC sets with each result and clears
 *        on a CPU read of an ADC_JDRy, JEOS sets with the last, and with JEXTEN 0 JADSTART clears
 *        with JEOS. ALIGN aligns ADC_JDRy as ADC_DR. AWD1 guards the injected results with
 *        JAWD1EN, AWD2 and AWD3 their channels. With JDISCEN each trigger converts the next
 *        channel, RM0351 18.4.20. With JDISCEN and JEXTEN 0 each JADSTART converts the next
 *        channel and JADSTART clears after it, inferred. JADSTP drops the injected conversion in
 *        progress and clears JADSTART, and the next start converts from JSQ1. ADDIS and ADCRST drop
 *        it too. A hardware trigger during an injected conversion is ignored, RM0351 18.4.18.
 *        JEXTSEL 8 and 10, the TRGO2 selections no L4 timer model produces, log a warning once, a
 *        model_error under strict_peripherals. A JADSTART write with ADEN 0 or ADDIS 1 is ignored,
 *        logged once, a model_error under strict_peripherals, RM0351 18.7.3. An ADC_JSQR write with
 *        JADSTART 1 and JQDIS 1 applies to the next sequence, logged once, a model_error under
 *        strict_peripherals, RM0351 18.7.16 as corrected by ES0250 2.8.2. An injected start or
 *        trigger during a regular conversion drops that conversion, RM0351 18.4.19. The group
 *        starts after tLATRINJ, DS10198 Table 76: at CKMODE 00 the next ADC clock edge then 2.5
 *        cycles, inside the 2.5 to 3.5 cycles of Table 76, inferred, and at CKMODE 01, 10 and 11
 *        the 3, 3.25 and 3.125 cycles Table 76 states as maxima. The dropped rank converts again
 *        from the end of the group, at once, inferred. A regular start with EXTEN 0 or a regular
 *        trigger during the injected group starts the regular sequence at its end, RM0351
 *        18.4.19, at once with no tLATR, inferred. ADSTART with EXTEN set arms the trigger. ADSTP
 *        during the group cancels the resumed rank and the latched start. JADSTP resumes the
 *        dropped rank at once, inferred.
 *        With JAUTO the injected group converts after the last regular rank, RM0351 18.4.19,
 *        with no delay, inferred, and with CONT the regular sequence starts again at JEOS. With
 *        CONT 0 and EXTEN 0 ADSTART clears at EOS, as the RM0351 18.7.3 ADSTART bit says, and
 *        JADSTART reads 0 while the auto-injected group converts, inferred: the RM0351 18.4.15
 *        note says ADSTART and JADSTART "provide information on whether any ADC operation is
 *        currently ongoing". ADSTP stops both groups, RM0351 18.4.17. The one-shot DMA transfer
 *        end stops the sequence and drops the conversion in progress, RM0351 18.4.19 and
 *        18.4.26, and clears ADSTART, inferred. JADSTART or JADSTP with JAUTO is ignored, JAUTO
 *        with JEXTEN set ignores the injected triggers, JAUTO with DISCEN or JDISCEN converts as
 *        JAUTO alone, and JAUTO with ROVSE 1 and JOVSE 0 oversamples the regular ranks only,
 *        RM0351 18.4.30, each logged once, a model_error under strict_peripherals.
 *        With JQDIS 0 a CPU or DMA ADC_JSQR write goes through the queue of context, RM0351
 *        18.4.21, with the one stage of ES0250 2.8.1 at JQM 0: a write before the waiting context
 *        is consumed sets JQOVF and is ignored. ADC_JSQR reads the active context. JEOS consumes
 *        it, and with JDISCEN the first trigger does, RM0351 18.4.21 note: a write after that waits
 *        and becomes the context at JEOS. Otherwise a write after the consumption applies at once,
 *        RM0351 Figure 84, and so does the first write after JADSTP or ADDIS drop the wait, Figures
 *        86, 88 and 90, or after ADCRST, inferred. A set or reset of JQDIS flushes the queue and
 *        clears ADC_JSQR, RM0351 18.7.4. A debugger write stores the word with no queue effect.
 *        Only hardware triggers start a sequence with the queue on, RM0351 18.4.21: JADSTART with
 *        JEXTEN 0 is ignored, logged once, a model_error under strict_peripherals.
 *        With JQM 1 the queue holds two contexts: a write to the empty queue applies at once, a
 *        later one waits, and a third sets JQOVF. JEOS applies the next context or empties the
 *        queue, which then reads ADC_JSQR 0, RM0351 Figures 85, 89 and 91, and ignores the injected
 *        triggers until a write. JADSTP, ADDIS and a JQDIS change empty it.
 *        JOVSE oversamples the injected ranks with OVSR and OVSS, RM0351 18.4.30, with ALIGN and
 *        the offsets ignored, and AWD1 then compares ADC_JDRy[15:4], the ADC_DR[15:4] rule of
 *        RM0351 18.4.30, inferred. With ROVSE and JOVSE an injection restarts the dropped regular
 *        rank from its first sample, the resumed mode of Figure 115, and JAUTO with both
 *        oversamples both groups, Figure 117. ROVSE and JOVSE with ROVSM 0 and JAUTO 0 convert in
 *        the resumed mode, as the RM0351 18.4.30 text says ROVSM is ignored there, and log a
 *        warning once, a model_error under strict_peripherals, as Table 119 marks the setting not
 *        supported. With ROVSE, ROVSM 0 and JOVSE 0 an injection keeps the samples the dropped rank
 *        completed, and the rank converts its remaining samples after the group, the sample in
 *        progress again, the continued mode of RM0351 18.4.30 Figure 114. TROVS with ROVSE converts
 *        one sample per trigger and lands the result after 2 to the OVSR + 1 triggers, Figure 113,
 *        with DISCEN read as 1 and one rank per trigger. TROVS with CONT is the DISCEN and CONT
 *        pair, logged once, a model_error under strict_peripherals, and converts as DISCEN
 *        alone. An injection between triggers zeroes the samples taken, and a sample it drops
 *        converts again at the end of the group with no trigger, the resumed mode of Figure 116.
 *        ADSTP between triggers zeroes them too, inferred. With EXTEN 0 each ADSTART converts one
 *        sample and ADSTART clears after it, inferred, and the samples taken survive an
 *        ADC_CFGR2, ADC_SQRx or ADC_SMPRx write between them, inferred. TROVS with ROVSM 0, and
 *        TROVS with JOVSE outside JAUTO, not supported by Table 119, log a warning once, a
 *        model_error under strict_peripherals, and convert one sample per trigger in the resumed
 *        mode. TROVS with JAUTO logs a warning once as not modelled. A strict model_error raised
 *        at a JADSTART write leaves ADC_CR as it was before the write.
 */
template <typename Traits>
class adc_model final : public dispatch_model<adc_model<Traits>, typename Traits::state>,
                        public analog_port,
                        public clock_listener,
                        public dma_requester,
                        public trigger_sink,
                        public bus_reset_listener
{
  using base = dispatch_model<adc_model<Traits>, typename Traits::state>;
  static_assert(not Traits::filter_gates_dma or not Traits::filter_field.empty());
  using base::claims;
  using base::ctx_;
  using base::desc_;
  using base::describe;
  using base::field;
  using base::irq;
  using base::reg;
  using base::state_;
  using base::warn;

  static constexpr bool injected_group = not std::is_void_v<typename Traits::injected_enum>;

public:
  adc_model(model_context& ctx, peripheral_desc const& desc, supply_options supply);

  void               reset(register_file& regs, reset_kind kind) override;
  void               load(std::span<std::byte const> in) override;
  void               on_bus_reset() override;
  void               on_event(std::uint32_t kind, std::uint64_t payload) override;
  void               on_clock_change() override;
  void               transfer_completed(dma_controller const& by, std::uint32_t request) override;
  [[nodiscard]] auto listens(std::string_view signal) const -> bool override;
  [[nodiscard]] auto can_listen(std::string_view signal) const -> bool override;
  void triggered(std::string_view signal, trigger_edge edge, trigger_time at) override;

  auto on_read(register_file& regs, std::uint32_t offset, std::uint8_t size, access_ctx ctx)
      -> std::expected<std::uint32_t, bus_error> override;
  auto on_write(
      register_file& regs,
      std::uint32_t  offset,
      std::uint8_t   size,
      std::uint32_t  old,
      std::uint32_t  value,
      access_ctx     ctx) -> std::expected<void, bus_error> override;

  void               drive(analog_channel channel, std::uint32_t microvolts) override;
  [[nodiscard]] auto input(analog_channel channel) const -> std::uint32_t override;
  [[nodiscard]] auto external_channels(analog_bank bank) const -> std::uint32_t override;
  [[nodiscard]] auto bonded_channels(analog_bank bank) const -> std::uint32_t override
  {
    return bank == analog_bank::a ? bonded_channels_ : 0;
  }
  [[nodiscard]] auto vdda_mv() const -> std::uint32_t override;

  void cr_written(std::uint32_t old, std::uint32_t now);
  void isr_written(std::uint32_t old, std::uint32_t now);
  void plain_written(std::uint32_t, std::uint32_t)
  {
    signal();
  }
  void dr_written(std::uint32_t, std::uint32_t)
  {}

private:
  friend class adc_common_model<Traits>;

  auto write_word(
      register_file& regs,
      std::uint32_t  offset,
      std::uint8_t   size,
      std::uint32_t  old,
      std::uint32_t  value,
      access_ctx     ctx) -> std::expected<void, bus_error>;
  void               restore_reset_values();
  [[nodiscard]] auto flags() const -> std::uint32_t;
  [[nodiscard]] auto common() const -> adc_common_model<Traits> const*;
  [[nodiscard]] auto source() -> adc_clock;
  [[nodiscard]] auto adc_hz() -> std::uint64_t;
  [[nodiscard]] auto optional_field(bool present, std::string_view reg, std::string_view bits) const
      -> std::optional<field_ref>;
  [[nodiscard]] auto oversampled() const -> bool;
  [[nodiscard]] auto alignment_ignored() const -> bool;
  [[nodiscard]] auto sampling() const -> std::uint32_t;
  [[nodiscard]] auto conversion() const -> std::uint32_t;
  [[nodiscard]] auto code() const -> std::uint32_t;
  [[nodiscard]] auto after(std::uint64_t half_cycles) const -> std::uint64_t;
  void               hold_until(std::uint64_t ps);
  [[nodiscard]] auto ratio() const -> std::uint32_t;
  [[nodiscard]] auto injected_oversampled() const -> bool;
  [[nodiscard]] auto triggered() const -> bool;
  [[nodiscard]] auto due_samples() const -> std::uint32_t;
  [[nodiscard]] auto result_oversampled() const -> bool;
  [[nodiscard]] auto shifted(std::uint32_t sum) const -> std::uint32_t;
  [[nodiscard]] auto channel() const -> std::uint32_t;
  [[nodiscard]] auto channel_at(std::size_t rank) const -> std::uint32_t;
  [[nodiscard]] auto length() const -> std::size_t;
  [[nodiscard]] auto busy() const -> bool;
  [[nodiscard]] auto external(analog_channel channel) const -> std::size_t;
  [[nodiscard]] auto is_external(std::uint32_t channel) const -> bool;
  [[nodiscard]] auto calibration(address at, std::string_view name) const -> std::uint16_t;
  [[nodiscard]] auto sample(std::uint32_t channel) const -> std::uint32_t;
  [[nodiscard]] auto dma() const -> dma_mode;
  [[nodiscard]] auto dma_level() const -> bool;
  [[nodiscard]] auto listened() const -> std::optional<std::uint32_t>;
  [[nodiscard]] auto regular_listens(std::string_view signal) const -> bool;
  void               retune(std::optional<std::uint32_t> was);
  [[nodiscard]] auto injecting() const -> bool;
  [[nodiscard]] auto injected_length() const -> std::size_t;
  [[nodiscard]] auto injected_listened() const -> std::optional<std::uint32_t>;
  [[nodiscard]] auto injected_listens(std::string_view signal) const -> bool;
  void               injected_notify(std::optional<std::uint32_t> selection);
  void               retune_injected(std::optional<std::uint32_t> was);
  void injected_triggered(std::string_view signal, trigger_edge edge, trigger_time at);
  void fire_injected(adc_edge& slot);
  void start_triggered_injected(std::uint64_t at_ps);
  void injected_written(std::uint32_t old, std::uint32_t now);
  void prepare_injected();
  void begin_injected(std::uint64_t start_ps);
  void finish_injected();
  void abort_injected();
  void latch_regular();
  [[nodiscard]] auto automatic() const -> bool;
  [[nodiscard]] auto queue_on() const -> bool;
  [[nodiscard]] auto emptying() const -> bool;
  void               apply_context(std::uint32_t word, bool empty = false);
  void               advance_queue();
  void               flush_queue();
  [[nodiscard]] auto regular_discontinuous() const -> bool;
  [[nodiscard]] auto injected_discontinuous() const -> bool;
  void               check_automatic();
  void               preempt(std::uint64_t at_ps);
  void               resume_regular(std::uint64_t at_ps);
  [[nodiscard]] auto aligned(std::uint32_t channel, std::uint32_t result) const -> std::uint32_t;
  [[nodiscard]] auto trigger_latency_ps(std::uint64_t at_ps, bool aborting = false)
      -> std::uint64_t;
  void               require_factory_word(std::uint32_t channel) const;
  void               prepare();
  void               begin_sequence(std::uint64_t start_ps);
  void               fire_edge(adc_edge& slot);
  void               begin_rank(std::uint64_t start_ps);
  void               schedule(std::uint64_t start_ps);
  void               keep_samples(std::uint64_t at_ps);
  void               schedule_sampling_end(std::uint64_t not_before_ps);
  void               finish();
  void               advance(std::uint64_t end_ps, bool last);
  [[nodiscard]] auto guarded(std::size_t watchdog, std::uint32_t channel) const -> bool;
  void               watch(std::uint32_t channel, std::uint32_t result);
  void               land(std::uint32_t result);
  void               pop();
  void               abort();
  void               signal();
  void               check_clock_range();
  void               check_clock_mode();
  void               check_unmodelled(std::uint32_t channel);
  void               check_oversampling_modes();
  void               check_preselection(std::uint32_t channel);
  [[nodiscard]] auto option_switch_set() const -> bool;
  void               check_option_switch(std::uint32_t channel);
  [[nodiscard]] auto option_open(std::uint32_t channel) const -> bool;
  [[nodiscard]] auto switch_open(std::uint32_t channel) const -> std::optional<adc_pad>;
  void               check_analog_switch(std::uint32_t channel);
  [[nodiscard]] auto pad_of(std::uint32_t channel) const -> adc_pad const*;
  [[nodiscard]] auto pad_port(char letter) const -> gpio_port*;
  [[nodiscard]] auto driven_pad(std::size_t channel) const -> std::pair<gpio_port&, std::uint8_t>;
  [[nodiscard]] auto level_uv(std::uint32_t channel) const -> std::uint32_t;
  void               listen_pads();
  [[nodiscard]] auto calibration_written(std::uint32_t old, std::uint32_t now) -> std::uint32_t;
  void               start_calibration();
  void               end_calibration();
  void               warn_once(bool& logged, std::string_view reg, std::string_view what);
  void               unmodelled(bool& logged, std::string_view name, std::string_view what);
  void               forbidden(bool& logged, std::string_view name, std::string message);
  void               refuse_start(bool refused);

  register_file&                                   regs_;
  supply_options                                   supply_;
  std::uint32_t                                    bonded_channels_{};
  adc_inputs                                       inputs_;
  std::vector<adc_pad>                             pads_;
  std::vector<subscription>                        pad_listeners_;
  core::clock_domain                               kernel_;
  core::clock_domain                               hclk_;
  core::clock_domain                               sysclk_;
  irq_line&                                        line_;
  dma_signal                                       request_;
  reg_ref                                          isr_;
  field_ref                                        enable_;
  field_ref                                        disable_;
  field_ref                                        start_;
  field_ref                                        stop_;
  field_ref                                        injected_start_;
  field_ref                                        regulator_;
  field_ref                                        deep_power_down_;
  field_ref                                        calibrate_;
  field_ref                                        ready_;
  field_ref                                        end_of_sampling_;
  field_ref                                        end_of_conversion_;
  field_ref                                        end_of_sequence_;
  field_ref                                        overrun_;
  std::optional<field_ref>                         regulator_ready_;
  std::array<field_ref, Traits::dma_fields.size()> dma_mode_;
  field_ref                                        resolution_;
  field_ref                                        trigger_;
  field_ref                                        trigger_select_;
  field_ref                                        overrun_mode_;
  field_ref                                        continuous_;
  field_ref                                        auto_delay_;
  field_ref                                        discontinuous_;
  field_ref                                        discontinuous_count_;
  typename Traits::sequence                        sequence_;
  field_ref                                        data_;
  field_ref                                        oversampling_;
  field_ref                                        oversampling_ratio_;
  field_ref                                        oversampling_shift_;
  std::optional<field_ref>                         low_frequency_;
  std::optional<field_ref>                         left_shift_;
  std::optional<field_ref>                         gain_;
  std::optional<field_ref>                         align_;
  std::optional<field_ref>                         injected_oversampling_;
  std::optional<field_ref>                         sampling_plus_;
  std::optional<field_ref>                         option_switch_;
  std::optional<field_ref>                         bulb_;
  std::optional<field_ref>                         sampling_trigger_;
  field_ref                                        triggered_oversampling_;
  reg_ref                                          differential_;
  std::optional<field_ref>                         differential_field_;
  std::optional<field_ref>                         calibration_mode_;
  std::optional<field_ref>                         factor_single_;
  std::optional<field_ref>                         factor_differential_;
  std::array<field_ref, channels>                  sampling_;
  std::array<offset_fields, offsets>               offsets_;
  std::optional<std::array<field_ref, channels>>   preselection_;
  field_ref                                        watchdog1_;
  field_ref                                        watchdog1_single_;
  field_ref                                        watchdog1_channel_;
  std::optional<field_ref>                         watchdog1_filter_;
  std::array<field_ref, watchdogs>                 watchdog_flags_;
  std::array<field_ref, watchdogs>                 watchdog_low_;
  std::array<field_ref, watchdogs>                 watchdog_high_;
  std::array<field_ref, watchdogs>                 watchdog_channels_;
  std::optional<injected_fields>                   injected_;
  std::vector<std::pair<field_ref, field_ref>>     interrupts_;
  std::uint32_t                                    set_only_{};
  std::uint32_t                                    clearable_{};
  std::array<bool, trigger_selections>             unproduced_logged_{};
  bool                                             injected_logged_{};
  bool                                             no_clock_logged_{};
  bool                                             auto_delay_logged_{};
  bool                                             low_frequency_logged_{};
  bool                                             mdf_logged_{};
  bool                                             start_refused_logged_{};
  bool                                             discontinuous_continuous_logged_{};
  bool                                             left_shift_logged_{};
  bool                                             offset_logged_{};
  bool                                             gain_logged_{};
  bool                                             watchdog_filter_dma_logged_{};
  bool                                             preselection_logged_{};
  bool                                             option_switch_logged_{};
  bool                                             clock_mode_logged_{};
  bool                                             bulb_logged_{};
  bool                                             sampling_trigger_logged_{};
  bool                                             triggered_oversampling_logged_{};
  bool                                             differential_logged_{};
  bool                                             switch_logged_{};
  bool                                             calibration_refused_logged_{};
  bool                                             enable_lost_logged_{};
  std::array<bool, trigger_selections>             injected_unproduced_logged_{};
  bool                                             injected_refused_logged_{};
  bool                                             queue_software_logged_{};
  bool                                             automatic_start_logged_{};
  bool                                             automatic_trigger_logged_{};
  bool                                             automatic_discontinuous_logged_{};
  bool                                             automatic_oversampling_logged_{};
  bool                                             oversampling_modes_logged_{};
  bool                                             triggered_modes_logged_{};
  bool                                             triggered_injected_logged_{};
  bool                                             injected_oversampling_logged_{};
  bool                                             sequence_written_logged_{};
};

template <typename Traits>
constexpr std::array<write_entry<adc_model<Traits>>, 5> adc_writes{
    {
     {"ADC_ISR", &adc_model<Traits>::isr_written},
     {"ADC_IER", &adc_model<Traits>::plain_written},
     {"ADC_CR", &adc_model<Traits>::cr_written},
     {Traits::config, &adc_model<Traits>::plain_written},
     {"ADC_DR", &adc_model<Traits>::dr_written},
     }
};

template <typename Traits>
constexpr std::array<write_entry<adc_model<Traits>>, 10> adc_injected_writes{
    {
     {"ADC_ISR", &adc_model<Traits>::isr_written},
     {"ADC_IER", &adc_model<Traits>::plain_written},
     {"ADC_CR", &adc_model<Traits>::cr_written},
     {Traits::config, &adc_model<Traits>::plain_written},
     {"ADC_DR", &adc_model<Traits>::dr_written},
     {"ADC_JSQR", nullptr},
     {"ADC_JDR1", nullptr},
     {"ADC_JDR2", nullptr},
     {"ADC_JDR3", nullptr},
     {"ADC_JDR4", nullptr},
     }
};

template <typename Traits> [[nodiscard]] constexpr auto adc_write_table()
{
  if constexpr(std::is_void_v<typename Traits::injected_enum>)
  {
    return std::span<write_entry<adc_model<Traits>> const>{adc_writes<Traits>};
  }
  else
  {
    return std::span<write_entry<adc_model<Traits>> const>{adc_injected_writes<Traits>};
  }
}

constexpr std::array<std::string_view, offsets> offset_registers{
    "ADC_OFR1", "ADC_OFR2", "ADC_OFR3", "ADC_OFR4"};

template <typename Traits>
adc_model<Traits>::adc_model(model_context& ctx, peripheral_desc const& desc, supply_options supply)
    : base{ctx, desc, Traits::unit_name, adc_write_table<Traits>()}, regs_{ctx.regs},
      supply_{supply}, inputs_{wiring<Traits>(desc.name)},
      kernel_{ctx.clock.domain(lower(desc.name))}, hclk_{ctx.clock.domain("hclk")},
      sysclk_{ctx.clock.domain("sysclk")},
      line_{irq(inputs_.irq.empty() ? desc.name + "_IRQn" : std::string{inputs_.irq})},
      request_{ctx.peers, lower(desc.name) + "_dma"}, isr_{reg("ADC_ISR")},
      enable_{field("ADC_CR", "ADEN")}, disable_{field("ADC_CR", "ADDIS")},
      start_{field("ADC_CR", "ADSTART")}, stop_{field("ADC_CR", "ADSTP")},
      injected_start_{field("ADC_CR", "JADSTART")}, regulator_{field("ADC_CR", "ADVREGEN")},
      deep_power_down_{field("ADC_CR", "DEEPPWD")}, calibrate_{field("ADC_CR", "ADCAL")},
      ready_{field("ADC_ISR", "ADRDY")}, end_of_sampling_{field("ADC_ISR", "EOSMP")},
      end_of_conversion_{field("ADC_ISR", "EOC")}, end_of_sequence_{field("ADC_ISR", "EOS")},
      overrun_{field("ADC_ISR", "OVR")},
      regulator_ready_{
          Traits::regulator_flag ? std::optional{field("ADC_ISR", "LDORDY")} : std::nullopt},
      resolution_{field(Traits::config, "RES")}, trigger_{field(Traits::config, "EXTEN")},
      trigger_select_{field(Traits::config, "EXTSEL")},
      overrun_mode_{field(Traits::config, "OVRMOD")}, continuous_{field(Traits::config, "CONT")},
      auto_delay_{field(Traits::config, "AUTDLY")}, discontinuous_{field(Traits::config, "DISCEN")},
      discontinuous_count_{field(Traits::config, "DISCNUM")},
      sequence_{[this](std::string_view name, std::string_view bits) { return field(name, bits); }},
      data_{field("ADC_DR", "RDATA")}, oversampling_{field("ADC_CFGR2", "ROVSE")},
      oversampling_ratio_{field("ADC_CFGR2", Traits::ratio_field)},
      oversampling_shift_{field("ADC_CFGR2", "OVSS")},
      low_frequency_{optional_field(Traits::low_frequency, "ADC_CFGR2", "LFTRIG")},
      left_shift_{optional_field(Traits::left_shift, "ADC_CFGR2", "LSHIFT")},
      gain_{optional_field(Traits::gain, "ADC_GCOMP", "GCOMP")},
      align_{optional_field(not Traits::align.empty(), Traits::config, Traits::align)},
      injected_oversampling_{optional_field(
          not Traits::injected_oversample.empty(),
          "ADC_CFGR2",
          Traits::injected_oversample)},
      sampling_plus_{
          optional_field(not Traits::sampling_plus.empty(), "ADC_SMPR1", Traits::sampling_plus)},
      option_switch_{optional_field(
          not Traits::option_switch.empty(),
          Traits::option_register,
          Traits::option_switch)},
      bulb_{optional_field(Traits::bulb, "ADC_CFGR2", "BULB")},
      sampling_trigger_{optional_field(Traits::sampling_trigger, "ADC_CFGR2", "SMPTRIG")},
      triggered_oversampling_{field("ADC_CFGR2", "TROVS")}, differential_{reg("ADC_DIFSEL")},
      differential_field_{
          optional_field(not Traits::differential.empty(), "ADC_DIFSEL", Traits::differential)},
      calibration_mode_{
          optional_field(Traits::calibration_cycles.has_value(), "ADC_CR", "ADCALDIF")},
      factor_single_{
          optional_field(Traits::calibration_cycles.has_value(), "ADC_CALFACT", "CALFACT_S")},
      factor_differential_{
          optional_field(Traits::calibration_cycles.has_value(), "ADC_CALFACT", "CALFACT_D")},
      watchdog1_{field(Traits::config, "AWD1EN")},
      watchdog1_single_{field(Traits::config, "AWD1SGL")},
      watchdog1_channel_{field(Traits::config, "AWD1CH")}, watchdog1_filter_{optional_field(
                                                               not Traits::filter_field.empty(),
                                                               Traits::filter_register,
                                                               Traits::filter_field)}
{
  bonded_channels_ = desc.bonded_channels.value_or(external_channels(analog_bank::a));
  if((bonded_channels_ & ~external_channels(analog_bank::a)) != 0)
  {
    throw config_error(desc.name, 0, "bonded_channels", "a mask of external channels");
  }
  if(desc.bonded_channels_b.value_or(0) != 0)
  {
    throw config_error(desc.name, 0, "bonded_channels_b", "0 or absent, the ADC has one bank");
  }
  require_channel_pins(desc, external_channels(analog_bank::a), pad_role::positive);
  for(auto const& pad : desc.channel_pins.value_or(std::vector<adc_pad_desc>{}))
  {
    if(pad.role == pad_role::positive and not pad.port.empty())
    {
      pads_.push_back({.channel = pad.channel, .port = pad.port.front(), .pin = pad.pin});
    }
  }
  for(std::size_t index = 0; index < Traits::dma_fields.size(); ++index)
  {
    dma_mode_[index] = field(Traits::config, Traits::dma_fields[index]);
  }
  for(std::size_t index = 0; index < watchdogs; ++index)
  {
    auto const  number     = index + 1;
    auto const& names      = Traits::thresholds[index];
    watchdog_flags_[index] = field("ADC_ISR", std::format("AWD{}", number));
    watchdog_low_[index]   = field(names.low_register, names.low);
    watchdog_high_[index]  = field(names.high_register, names.high);
    if(index != 0)
    {
      watchdog_channels_[index] =
          field(std::format("ADC_AWD{}CR", number), std::format("AWD{}CH", number));
    }
  }
  if constexpr(Traits::preselection)
  {
    preselection_.emplace();
  }
  for(std::size_t index = 0; index < Traits::channel_count; ++index)
  {
    if(preselection_)
    {
      (*preselection_)[index] = field("ADC_PCSEL", std::format("PCSEL{}", index));
    }
    sampling_[index] = field(index < 10 ? "ADC_SMPR1" : "ADC_SMPR2", std::format("SMP{}", index));
  }
  for(std::size_t index = 0; index < offsets; ++index)
  {
    auto const name = offset_registers[index];
    offsets_[index] = {
        .reg     = name,
        .value   = field(name, "OFFSET"),
        .channel = field(name, "OFFSET_CH"),
        .enable  = optional_field(not Traits::offset_enable.empty(), name, Traits::offset_enable)};
  }
  if constexpr(injected_group)
  {
    injected_fields fields{
        .stop                 = field("ADC_CR", "JADSTP"),
        .length               = field("ADC_JSQR", "JL"),
        .trigger              = field("ADC_JSQR", "JEXTEN"),
        .select               = field("ADC_JSQR", "JEXTSEL"),
        .discontinuous        = field(Traits::config, "JDISCEN"),
        .queue_disable        = field(Traits::config, "JQDIS"),
        .queue_mode           = field(Traits::config, "JQM"),
        .queue_overflow       = field("ADC_ISR", "JQOVF"),
        .automatic            = field(Traits::config, "JAUTO"),
        .watchdog1            = field(Traits::config, "JAWD1EN"),
        .resumed_oversampling = field("ADC_CFGR2", "ROVSM"),
        .end_of_conversion    = field("ADC_ISR", "JEOC"),
        .end_of_sequence      = field("ADC_ISR", "JEOS"),
        .sequence_register    = reg("ADC_JSQR"),
        .sequence             = {},
        .data                 = {}};
    for(std::size_t rank = 0; rank < injected_ranks; ++rank)
    {
      fields.sequence[rank] = field("ADC_JSQR", std::format("JSQ{}", rank + 1));
      fields.data[rank]     = field(std::format("ADC_JDR{}", rank + 1), "JDATA");
    }
    injected_.emplace(fields);
  }
  for(auto const bit : {enable_, disable_, start_, stop_, injected_start_, calibrate_})
  {
    set_only_ |= bit.mask();
  }
  set_only_ |= field("ADC_CR", "JADSTP").mask();
  auto const& status = describe("ADC_ISR");
  for(auto const& flag : status.fields)
  {
    if(flag.on_write == write_effect::one_to_clear)
    {
      clearable_ |= field("ADC_ISR", flag.name).mask();
    }
  }
  for(auto const& enable : describe("ADC_IER").fields)
  {
    auto const flag = std::string_view{enable.name}.substr(0, enable.name.size() - 2);
    if(enable.name.ends_with("IE")
       and std::ranges::contains(status.fields, flag, &field_desc::name))
    {
      interrupts_.emplace_back(field("ADC_ISR", flag), field("ADC_IER", enable.name));
    }
  }
}

template <typename Traits> auto adc_model<Traits>::common() const -> adc_common_model<Traits> const*
{
  return dynamic_cast<adc_common_model<Traits> const*>(ctx_.peers.find(Traits::common_unit));
}

template <typename Traits> auto adc_model<Traits>::source() -> adc_clock
{
  auto* const shared =
      dynamic_cast<adc_common_model<Traits>*>(ctx_.peers.find(Traits::common_unit));
  if constexpr(not Traits::common_clock_mode.empty())
  {
    if(shared != nullptr and shared->clock_mode() != 0)
    {
      return {
          .hz      = ctx_.clock.hz(hclk_),
          .divider = Traits::synchronous_dividers[shared->clock_mode() - 1u]};
    }
  }
  auto const hz = ctx_.clock.hz(kernel_);
  if(hz == 0)
  {
    return {.hz = 0, .divider = 1};
  }
  return {.hz = hz, .divider = shared != nullptr ? shared->divider() : 1u};
}

template <typename Traits> auto adc_model<Traits>::adc_hz() -> std::uint64_t
{
  auto const clock = source();
  return clock.hz / clock.divider;
}

template <typename Traits>
auto adc_model<Traits>::optional_field(bool present, std::string_view reg, std::string_view bits)
    const -> std::optional<field_ref>
{
  return present ? std::optional{field(reg, bits)} : std::nullopt;
}

template <typename Traits> auto adc_model<Traits>::oversampled() const -> bool
{
  return get(regs_, oversampling_) != 0;
}

template <typename Traits> auto adc_model<Traits>::alignment_ignored() const -> bool
{
  return oversampled() or (injected_oversampling_ and get(regs_, *injected_oversampling_) != 0);
}

template <typename Traits> auto adc_model<Traits>::flags() const -> std::uint32_t
{
  return regs_.peek(isr_.offset);
}

template <typename Traits> auto adc_model<Traits>::sampling() const -> std::uint32_t
{
  auto const which = channel();
  auto const code  = which < Traits::channel_count ? get(regs_, sampling_[which]) : 0u;
  if constexpr(not Traits::sampling_plus.empty())
  {
    if(code == 0 and get(regs_, *sampling_plus_) != 0)
    {
      return Traits::sampling_plus_half;
    }
  }
  return Traits::sampling_half_cycles[code];
}

template <typename Traits> auto adc_model<Traits>::conversion() const -> std::uint32_t
{
  return sampling() + Traits::sar_half_cycles[get(regs_, resolution_)];
}

template <typename Traits> auto adc_model<Traits>::code() const -> std::uint32_t
{
  return sample(channel()) >> (2 * get(regs_, resolution_));
}

template <typename Traits>
auto adc_model<Traits>::after(std::uint64_t half_cycles) const -> std::uint64_t
{
  return state_.start_ps + half_cycles_ps(half_cycles, state_.divider, state_.clock_hz);
}

template <typename Traits> void adc_model<Traits>::hold_until(std::uint64_t ps)
{
  auto const samples = ratio();
  while(state_.held < samples
        and after(std::uint64_t{state_.held} * conversion() + sampling()) <= ps)
  {
    auto const sampled = code();
    if constexpr(injected_group)
    {
      state_.last_code = sampled;
    }
    state_.sum += sampled;
    ++state_.held;
  }
}

template <typename Traits> auto adc_model<Traits>::ratio() const -> std::uint32_t
{
  return result_oversampled() ? Traits::ratio(get(regs_, oversampling_ratio_)) : 1u;
}

template <typename Traits> auto adc_model<Traits>::triggered() const -> bool
{
  return Traits::triggered_ovs and oversampled() and get(regs_, triggered_oversampling_) != 0;
}

template <typename Traits> auto adc_model<Traits>::due_samples() const -> std::uint32_t
{
  if constexpr(injected_group)
  {
    if(triggered() and not injecting())
    {
      return state_.kept_held + 1u;
    }
  }
  return ratio();
}

template <typename Traits> auto adc_model<Traits>::injected_oversampled() const -> bool
{
  return injected_oversampling_ and get(regs_, *injected_oversampling_) != 0;
}

template <typename Traits> auto adc_model<Traits>::result_oversampled() const -> bool
{
  if(injecting())
  {
    return Traits::injected_ovs and injected_oversampled();
  }
  return oversampled();
}

template <typename Traits> auto adc_model<Traits>::shifted(std::uint32_t sum) const -> std::uint32_t
{
  auto const shift = get(regs_, oversampling_shift_);
  return (shift == 0 ? sum : (sum + (1u << (shift - 1u))) >> shift) & Traits::result_mask;
}

template <typename Traits> auto adc_model<Traits>::channel() const -> std::uint32_t
{
  if constexpr(injected_group)
  {
    if(state_.injecting)
    {
      return get(regs_, injected_->sequence[state_.injected_rank]);
    }
  }
  return channel_at(state_.rank);
}

template <typename Traits>
auto adc_model<Traits>::channel_at(std::size_t rank) const -> std::uint32_t
{
  return sequence_.next_channel(regs_, rank);
}

template <typename Traits> auto adc_model<Traits>::length() const -> std::size_t
{
  return sequence_.sequence_length(regs_);
}

template <typename Traits> auto adc_model<Traits>::busy() const -> bool
{
  return state_.converting or state_.waiting_clock;
}

template <typename Traits>
auto adc_model<Traits>::external(analog_channel channel) const -> std::size_t
{
  if(channel.bank != analog_bank::a)
  {
    throw config_error(
        desc_.name, 0, "bank",
        std::format("bank A, the ADC has one bank, not channel {}b", channel.number));
  }
  if(not is_external(channel.number))
  {
    throw config_error(
        desc_.name, 0, "channel",
        std::format(
            "an external channel {}, not {}",
            inputs_.external_names.empty() ? Traits::externals : inputs_.external_names,
            channel.number));
  }
  return channel.number;
}

template <typename Traits>
void adc_model<Traits>::drive(analog_channel channel, std::uint32_t microvolts)
{
  auto const [port, pin] = driven_pad(external(channel));
  listen_pads();
  port.drive_analog(pin, microvolts);
}

template <typename Traits>
auto adc_model<Traits>::input(analog_channel channel) const -> std::uint32_t
{
  auto const [port, pin] = driven_pad(external(channel));
  return port.analog_drive(pin).value_or(0);
}

template <typename Traits>
auto adc_model<Traits>::external_channels(analog_bank bank) const -> std::uint32_t
{
  static constexpr auto mask = [] {
    std::uint32_t bits = 0;
    for(std::uint32_t channel = 0; channel < channels; ++channel)
    {
      if(Traits::external(channel))
      {
        bits |= std::uint32_t{1} << channel;
      }
    }
    return bits;
  }();
  if(bank != analog_bank::a)
  {
    return 0;
  }
  return inputs_.externals != 0 ? inputs_.externals : mask;
}

template <typename Traits> auto adc_model<Traits>::is_external(std::uint32_t channel) const -> bool
{
  if(inputs_.externals != 0)
  {
    return channel < channels and (inputs_.externals >> channel & 1u) != 0;
  }
  return Traits::external(channel);
}

template <typename Traits> auto adc_model<Traits>::vdda_mv() const -> std::uint32_t
{
  return supply_.vdda_mv;
}

template <typename Traits>
auto adc_model<Traits>::calibration(address at, std::string_view name) const -> std::uint16_t
{
  auto const word = factory_halfword(ctx_.part, at);
  if(not word)
  {
    throw config_error(
        ctx_.part.part, 0, std::format("regions.content.{:#010x}", at),
        std::format("the factory {} word the ADC model converts", name));
  }
  return *word;
}

template <typename Traits> void adc_model<Traits>::reset(register_file&, reset_kind)
{
  restore_reset_values();
  listen_pads();
}

template <typename Traits> void adc_model<Traits>::load(std::span<std::byte const> in)
{
  base::load(in);
  listen_pads();
}

template <typename Traits> void adc_model<Traits>::listen_pads()
{
  if(not pad_listeners_.empty())
  {
    return;
  }
  std::string ports;
  for(auto& pad : pads_)
  {
    pad.gpio          = pad_port(pad.port);
    pad.switch_source = dynamic_cast<analog_switch_source const*>(pad.gpio);
    if(not ports.contains(pad.port))
    {
      ports.push_back(pad.port);
    }
  }
  for(auto const letter : ports)
  {
    auto* const port = pad_port(letter);
    if(port == nullptr)
    {
      continue;
    }
    std::uint16_t pins = 0;
    for(auto const& pad : pads_)
    {
      pins |= pad.port == letter ? static_cast<std::uint16_t>(1u << pad.pin) : std::uint16_t{0};
    }
    pad_listeners_.push_back(port->on_analog_changing([this, pins](std::uint8_t pin) {
      if(state_.converting and (pins >> pin & 1u) != 0)
      {
        hold_until(ctx_.clock.time().ps);
      }
    }));
  }
}

template <typename Traits>
auto adc_model<Traits>::pad_of(std::uint32_t channel) const -> adc_pad const*
{
  auto const pad = std::ranges::find(pads_, channel, &adc_pad::channel);
  return pad == pads_.end() ? nullptr : &*pad;
}

template <typename Traits> auto adc_model<Traits>::pad_port(char letter) const -> gpio_port*
{
  return dynamic_cast<gpio_port*>(ctx_.peers.find(std::format("GPIO{}", letter)));
}

template <typename Traits>
auto adc_model<Traits>::driven_pad(std::size_t channel) const -> std::pair<gpio_port&, std::uint8_t>
{
  auto const* const pad  = pad_of(static_cast<std::uint32_t>(channel));
  auto* const       port = pad == nullptr       ? nullptr
                         : pad->gpio != nullptr ? pad->gpio
                                                : pad_port(pad->port);
  if(port == nullptr)
  {
    throw config_error(
        desc_.name, 0, std::format("channel_pins.{}", channel),
        std::format("a positive pad of channel {} on a GPIO port of the machine", channel));
  }
  return {*port, pad->pin};
}

template <typename Traits>
auto adc_model<Traits>::level_uv(std::uint32_t channel) const -> std::uint32_t
{
  auto const* const pad = pad_of(channel);
  return pad == nullptr or pad->gpio == nullptr ? 0 : pad->gpio->analog_level(pad->pin);
}

template <typename Traits> void adc_model<Traits>::on_bus_reset()
{
  auto const was = listened();
  if(was)
  {
    static_cast<void>(
        interest_changed(ctx_.peers, trigger_signal<typename Traits::trigger_enum>(*was)));
  }
  auto const injected_was = injected_listened();
  injected_notify(injected_was);
  restore_reset_values();
  retune(was);
  retune_injected(injected_was);
}

template <typename Traits> void adc_model<Traits>::restore_reset_values()
{
  for(auto const& name : claims())
  {
    regs_.poke(reg(name).offset, describe(name).reset);
  }
  abort();
  state_.data.clear();
  state_.dma_stopped  = false;
  state_.filter_count = 0;
  state_.edges        = {};
  ++state_.edge_generation;
  if constexpr(Traits::calibration_cycles.has_value())
  {
    ++state_.calibration_generation;
    state_.calibration_waiting = false;
    state_.enable_lock_ps      = 0;
  }
  if constexpr(injected_group)
  {
    abort_injected();
    state_.injected_edges = {};
    state_.queue_pending  = false;
    state_.queue_empty    = true;
    state_.queue_count    = 0;
  }
  request_.resolve();
  signal();
}

template <typename Traits>
auto adc_model<Traits>::on_read(
    register_file& regs,
    std::uint32_t  offset,
    std::uint8_t   size,
    access_ctx     ctx) -> std::expected<std::uint32_t, bus_error>
{
  auto const value = base::on_read(regs, offset, size, ctx);
  if((offset & ~3u) == data_.offset and ctx.by != initiator::debugger)
  {
    pop();
  }
  if constexpr(injected_group)
  {
    if(ctx.by != initiator::debugger
       and std::ranges::contains(injected_->data, offset & ~3u, &field_ref::offset))
    {
      update(regs_, injected_->end_of_conversion, 0);
      signal();
    }
  }
  return value;
}

template <typename Traits> void adc_model<Traits>::cr_written(std::uint32_t old, std::uint32_t now)
{
  now |= old & set_only_;
  auto const refused = start_.of(now) != 0
                   and start_.of(old) == 0
                   and (enable_.of(old) == 0 or disable_.of(now) != 0);
  if(refused)
  {
    now = start_.with(now, 0);
  }
  if constexpr(injected_group)
  {
    if(injected_start_.of(now) != 0
       and injected_start_.of(old) == 0
       and (enable_.of(old) == 0 or disable_.of(now) != 0))
    {
      now = injected_start_.with(now, 0);
      if(ctx_.strict_peripherals)
      {
        regs_.poke(enable_.offset, old);
      }
      forbidden(
          injected_refused_logged_, "ADC_CR",
          "JADSTART written with ADEN 0 or ADDIS 1, RM0351 18.7.3: the write is ignored");
    }
    auto const injected_rises = injected_start_.of(now) != 0 and injected_start_.of(old) == 0;
    if(automatic() and (injected_rises or injected_->stop.of(now) != 0))
    {
      now = injected_start_.with(now, injected_start_.of(old));
      now = injected_->stop.with(now, 0);
      if(ctx_.strict_peripherals)
      {
        regs_.poke(enable_.offset, old);
      }
      forbidden(
          automatic_start_logged_, "ADC_CR",
          "JADSTART or JADSTP with JAUTO, forbidden by RM0351 18.4.19: ignored, converted as "
          "JAUTO alone");
    }
    if(injected_start_.of(now) != 0
       and injected_start_.of(old) == 0
       and queue_on()
       and get(regs_, injected_->trigger) == 0)
    {
      now = injected_start_.with(now, 0);
      if(ctx_.strict_peripherals)
      {
        regs_.poke(enable_.offset, old);
      }
      forbidden(
          queue_software_logged_, "ADC_CR",
          "JADSTART with JQDIS 0 and JEXTEN 0, forbidden by RM0351 18.4.21: the queue of context "
          "starts only on hardware triggers, the write is ignored");
    }
  }
  if(deep_power_down_.of(now) != 0)
  {
    now = regulator_.with(now, 0);
  }
  auto lost = false;
  if constexpr(Traits::calibration_cycles.has_value())
  {
    now  = calibration_written(old, now);
    lost = enable_.of(now) != 0
       and enable_.of(old) == 0
       and (calibrate_.of(now) != 0 or ctx_.clock.time().ps < state_.enable_lock_ps);
    if(lost)
    {
      now = enable_.with(now, 0);
    }
  }
  regs_.poke(enable_.offset, now);
  if constexpr(Traits::regulator_flag)
  {
    update(
        regs_, *regulator_ready_,
        regulator_.of(now) != 0 and deep_power_down_.of(now) == 0 ? 1u : 0u);
  }
  if constexpr(not Traits::calibration_cycles.has_value())
  {
    update(regs_, calibrate_, 0);
  }
  if(lost)
  {
    forbidden(
        enable_lost_logged_, "ADC_CR",
        "ADEN set while ADCAL is set or within 4 ADC clock cycles after it clears, RM0351 18.4.9 "
        "as corrected by ES0250 2.8.4: ADEN is cleared and the ADC stays disabled");
  }
  if constexpr(not injected_group)
  {
    if(injected_start_.of(now) != 0)
    {
      update(regs_, injected_start_, 0);
      warn_once(injected_logged_, "ADC_CR", "JADSTART: injected conversions");
    }
  }
  if(disable_.of(now) != 0)
  {
    update(regs_, disable_, 0);
    update(regs_, enable_, 0);
    update(regs_, start_, 0);
    update(regs_, stop_, 0);
    if constexpr(injected_group)
    {
      update(regs_, injected_start_, 0);
      update(regs_, injected_->stop, 0);
      abort_injected();
      flush_queue();
    }
    abort();
    signal();
    refuse_start(refused);
    return;
  }
  if(enable_.of(now) != 0 and enable_.of(old) == 0)
  {
    if constexpr(Traits::ready_adc_cycles.has_value())
    {
      if(regulator_.of(now) != 0 and deep_power_down_.of(now) == 0)
      {
        auto const clock = source();
        if(clock.hz == 0)
        {
          if(not std::exchange(no_clock_logged_, true))
          {
            warn("ADC_CR", "no ADC kernel clock: ADRDY waits");
          }
        }
        else
        {
          ctx_.events.schedule_at(
              vtime{
                  ctx_.clock.time().ps
                  + cycles_ps(*Traits::ready_adc_cycles, 1, clock.divider, clock.hz)},
              ready_end, ++state_.ready_generation, ctx_.clock);
        }
      }
    }
    else if constexpr(Traits::ready_cycles.has_value())
    {
      if(deep_power_down_.of(now) == 0)
      {
        auto const cycles = (*Traits::ready_cycles)[regulator_.of(now) != 0 ? 0 : 1];
        ctx_.events.schedule_at(
            vtime{ctx_.clock.time().ps + cycles * ps_per_second / ctx_.clock.core_hz()}, ready_end,
            ++state_.ready_generation, ctx_.clock);
      }
    }
    else if(regulator_.of(now) != 0 and deep_power_down_.of(now) == 0)
    {
      update(regs_, ready_, 1);
    }
    check_clock_range();
    check_clock_mode();
  }
  if constexpr(injected_group)
  {
    injected_written(old, now);
  }
  if(stop_.of(now) != 0)
  {
    update(regs_, stop_, 0);
    update(regs_, start_, 0);
    if(automatic())
    {
      abort_injected();
    }
    abort();
    signal();
    return;
  }
  if(start_.of(now) != 0 and start_.of(old) == 0)
  {
    state_.dma_stopped = false;
    if(enable_.of(now) != 0 and injecting() and get(regs_, trigger_) == 0)
    {
      latch_regular();
    }
    if(enable_.of(now) != 0 and not busy())
    {
      update(regs_, start_, 0);
      if(get(regs_, trigger_) == 0)
      {
        begin_sequence(ctx_.clock.time().ps);
      }
      else
      {
        prepare();
      }
      update(regs_, start_, 1);
    }
  }
  signal();
  refuse_start(refused);
}

template <typename Traits>
auto adc_model<Traits>::calibration_written(std::uint32_t old, std::uint32_t now) -> std::uint32_t
{
  if(calibrate_.of(now) == 0 or calibrate_.of(old) != 0)
  {
    return now;
  }
  if(enable_.of(old) != 0 or deep_power_down_.of(now) != 0 or regulator_.of(now) == 0)
  {
    if(ctx_.strict_peripherals)
    {
      regs_.poke(enable_.offset, old);
    }
    forbidden(
        calibration_refused_logged_, "ADC_CR",
        "ADCAL written with ADEN 1, DEEPPWD 1 or ADVREGEN 0, RM0351 18.4.8: the write is ignored");
    return calibrate_.with(now, 0);
  }
  auto const factor = calibration_mode_->of(now) != 0 ? *factor_differential_ : *factor_single_;
  update(regs_, factor, 0);
  start_calibration();
  return now;
}

template <typename Traits> void adc_model<Traits>::start_calibration()
{
  auto const clock = source();
  if(clock.hz == 0)
  {
    if(not std::exchange(no_clock_logged_, true))
    {
      warn("ADC_CR", "no ADC kernel clock: the calibration waits");
    }
    state_.calibration_waiting = true;
    return;
  }
  state_.calibration_waiting = false;
  ctx_.events.schedule_at(
      vtime{
          ctx_.clock.time().ps
          + cycles_ps(*Traits::calibration_cycles, 1, clock.divider, clock.hz)},
      calibration_end, ++state_.calibration_generation, ctx_.clock);
}

template <typename Traits> void adc_model<Traits>::end_calibration()
{
  auto const clock = source();
  update(regs_, calibrate_, 0);
  state_.enable_lock_ps =
      ctx_.clock.time().ps
      + (clock.hz == 0 ? 0 : cycles_ps(Traits::enable_lockout, 1, clock.divider, clock.hz));
  signal();
}

template <typename Traits>
auto adc_model<Traits>::switch_open(std::uint32_t channel) const -> std::optional<adc_pad>
{
  auto const* const pad = pad_of(channel);
  if(pad == nullptr or pad->switch_source == nullptr or pad->switch_source->switch_closed(pad->pin))
  {
    return std::nullopt;
  }
  return *pad;
}

template <typename Traits> void adc_model<Traits>::check_analog_switch(std::uint32_t channel)
{
  if(auto const pad = switch_open(channel))
  {
    forbidden(
        switch_logged_, "ADC_SQR1",
        std::format(
            "channel {} converted with GPIO{}_ASCR.ASC{} clear, RM0351 18.4.11: converted as 0",
            channel, pad->port, pad->pin));
  }
}

template <typename Traits> void adc_model<Traits>::refuse_start(bool refused)
{
  if(refused)
  {
    forbidden(
        start_refused_logged_, "ADC_CR",
        std::format(
            "ADSTART written with ADEN 0 or ADDIS 1, {}: the write is ignored",
            Traits::cite_control));
  }
}

template <typename Traits> void adc_model<Traits>::isr_written(std::uint32_t old, std::uint32_t now)
{
  regs_.poke(isr_.offset, old & ~(now & clearable_));
  if(end_of_sampling_.of(old) != 0
     and get(regs_, end_of_sampling_) == 0
     and state_.converting
     and not injecting())
  {
    schedule_sampling_end(ctx_.clock.time().ps);
  }
  signal();
}

template <typename Traits> void adc_model<Traits>::check_clock_range()
{
  if(source().hz == 0)
  {
    return;
  }
  auto const hz = adc_hz();
  if(hz < Traits::min_hz or hz > Traits::max_hz)
  {
    warn("ADC_CR", std::format("ADC clock {} Hz is outside {}", hz, Traits::clock_range));
  }
}

template <typename Traits> void adc_model<Traits>::check_clock_mode()
{
  if constexpr(not Traits::common_clock_mode.empty())
  {
    auto const* const shared = common();
    if(shared != nullptr
       and shared->clock_mode() == 1
       and ctx_.clock.hz(hclk_) != ctx_.clock.hz(sysclk_))
    {
      forbidden(
          clock_mode_logged_, "ADC_CR",
          std::format(
              "CKMODE 01 with HCLK {} Hz not SYSCLK {} Hz, forbidden by {}: converted at HCLK",
              ctx_.clock.hz(hclk_), ctx_.clock.hz(sysclk_), Traits::cite_clock_mode));
    }
  }
}

template <typename Traits> void adc_model<Traits>::prepare()
{
  if constexpr(Traits::injected_ovs)
  {
    check_oversampling_modes();
  }
  if(get(regs_, discontinuous_) != 0 and get(regs_, continuous_) != 0)
  {
    forbidden(
        discontinuous_continuous_logged_, Traits::config,
        std::format(
            "DISCEN and CONT both set, forbidden by {}: converted as DISCEN alone",
            Traits::cite_discontinuous));
  }
  else if(triggered() and get(regs_, continuous_) != 0)
  {
    forbidden(
        discontinuous_continuous_logged_, Traits::config,
        std::format(
            "TROVS and CONT both set, DISCEN read as 1 by RM0351 18.4.30, forbidden by {}: "
            "converted as DISCEN alone",
            Traits::cite_discontinuous));
  }
  if(get(regs_, auto_delay_) != 0)
  {
    unmodelled(auto_delay_logged_, Traits::config, "AUTDLY: auto-delayed conversions");
  }
  if(bulb_ and get(regs_, *bulb_) != 0)
  {
    unmodelled(bulb_logged_, "ADC_CFGR2", "BULB: bulb sampling mode");
  }
  if(sampling_trigger_ and get(regs_, *sampling_trigger_) != 0)
  {
    unmodelled(
        sampling_trigger_logged_, "ADC_CFGR2", "SMPTRIG: sampling time control trigger mode");
  }
  if(get(regs_, triggered_oversampling_) != 0 and (not Traits::triggered_ovs or automatic()))
  {
    unmodelled(
        triggered_oversampling_logged_, "ADC_CFGR2", "TROVS: triggered regular oversampling");
  }
  if(low_frequency_ and get(regs_, *low_frequency_) != 0)
  {
    unmodelled(low_frequency_logged_, "ADC_CFGR2", "LFTRIG: low frequency trigger mode");
  }
  if(dma() == dma_mode::mdf)
  {
    unmodelled(mdf_logged_, Traits::config, "DMNGT 10: the MDF data path");
  }
  if constexpr(injected_group)
  {
    if(automatic())
    {
      check_automatic();
      prepare_injected();
    }
  }
  if constexpr(Traits::filter_gates_dma)
  {
    if(get(regs_, watchdog1_) != 0
       and get(regs_, *watchdog1_filter_) != 0
       and dma() != dma_mode::off)
    {
      unmodelled(
          watchdog_filter_dma_logged_, "ADC_HTR1",
          "AWDFILT1 with DMNGT: DMA requests gated by the AWD1 window");
    }
  }
  for(std::size_t rank = 0; rank < length(); ++rank)
  {
    check_unmodelled(channel_at(rank));
    check_preselection(channel_at(rank));
    check_option_switch(channel_at(rank));
    check_analog_switch(channel_at(rank));
    require_factory_word(channel_at(rank));
  }
}

template <typename Traits> void adc_model<Traits>::begin_sequence(std::uint64_t start_ps)
{
  prepare();
  if(not regular_discontinuous() or state_.rank >= length())
  {
    state_.rank = 0;
  }
  state_.subgroup_left = static_cast<std::uint8_t>(get(regs_, discontinuous_count_) + 1u);
  if constexpr(Traits::triggered_ovs)
  {
    if(triggered())
    {
      state_.subgroup_left = 1;
      begin_rank(start_ps);
      return;
    }
  }
  begin_rank(start_ps);
}

template <typename Traits> void adc_model<Traits>::begin_rank(std::uint64_t start_ps)
{
  if(source().hz == 0)
  {
    if(not std::exchange(no_clock_logged_, true))
    {
      warn("ADC_CR", "no ADC kernel clock: the conversion waits");
    }
    state_.waiting_clock = true;
    return;
  }
  schedule(start_ps);
}

template <typename Traits> void adc_model<Traits>::check_unmodelled(std::uint32_t channel)
{
  auto const differential =
      differential_field_ ? get(regs_, *differential_field_) : regs_.peek(differential_.offset);
  if(channel < channels and (differential >> channel & 1u) != 0)
  {
    unmodelled(differential_logged_, "ADC_DIFSEL", "DIFSEL: differential conversion");
  }
  if(left_shift_ and get(regs_, *left_shift_) != 0)
  {
    unmodelled(left_shift_logged_, "ADC_CFGR2", "LSHIFT: left shift of the result");
  }
  if(gain_ and get(regs_, *gain_) != 0)
  {
    unmodelled(gain_logged_, "ADC_GCOMP", "GCOMP: gain compensation");
  }
  if constexpr(not Traits::subtracts_offset)
  {
    for(auto const& offset : offsets_)
    {
      auto const active = offset.enable
                            ? get(regs_, *offset.enable) != 0 and not alignment_ignored()
                            : get(regs_, offset.value) != 0;
      if(active and get(regs_, offset.channel) == channel)
      {
        unmodelled(offset_logged_, offset.reg, "OFFSET: offset of the result");
      }
    }
  }
}

template <typename Traits> void adc_model<Traits>::check_oversampling_modes()
{
  if constexpr(injected_group)
  {
    if(oversampled()
       and injected_oversampled()
       and get(regs_, injected_->resumed_oversampling) == 0
       and not automatic())
    {
      forbidden(
          oversampling_modes_logged_, "ADC_CFGR2",
          "ROVSE 1, JOVSE 1 and ROVSM 0, not supported by RM0351 Table 119: converted in the "
          "resumed mode");
    }
    if(triggered() and not automatic() and get(regs_, injected_->resumed_oversampling) == 0)
    {
      forbidden(
          triggered_modes_logged_, "ADC_CFGR2",
          "ROVSE 1, TROVS 1 and ROVSM 0, not supported by RM0351 Table 119: converted in the "
          "triggered resumed mode");
    }
    if(triggered() and not automatic() and injected_oversampled())
    {
      forbidden(
          triggered_injected_logged_, "ADC_CFGR2",
          "ROVSE 1, JOVSE 1 and TROVS 1, not supported by RM0351 Table 119: the injected ranks are "
          "oversampled, the regular ranks one sample per trigger");
    }
  }
}

template <typename Traits> void adc_model<Traits>::check_preselection(std::uint32_t channel)
{
  if(preselection_ and channel < channels and get(regs_, (*preselection_)[channel]) == 0)
  {
    forbidden(
        preselection_logged_, "ADC_PCSEL",
        std::format(
            "channel {} converted with its PCSEL bit clear, RM0456 33.4.12: converted as if set",
            channel));
  }
}

template <typename Traits> auto adc_model<Traits>::option_switch_set() const -> bool
{
  return option_switch_ and get(regs_, *option_switch_) != 0;
}

template <typename Traits> auto adc_model<Traits>::option_open(std::uint32_t channel) const -> bool
{
  if constexpr(not Traits::option_switch.empty())
  {
    if(channel == Traits::option_channel)
    {
      auto const* const owner =
          dynamic_cast<adc_model const*>(ctx_.peers.find(Traits::option_owner));
      return owner == nullptr or not owner->option_switch_set();
    }
  }
  return false;
}

template <typename Traits> void adc_model<Traits>::check_option_switch(std::uint32_t channel)
{
  if constexpr(not Traits::option_switch.empty())
  {
    if(not option_open(channel))
    {
      return;
    }
    forbidden(
        option_switch_logged_, Traits::option_register,
        std::format(
            "channel {} converted with {} {}.{} clear, {}: converted as 0", channel,
            Traits::option_owner, Traits::option_register, Traits::option_switch,
            Traits::cite_option));
  }
}

template <typename Traits> void adc_model<Traits>::schedule(std::uint64_t start_ps)
{
  std::uint32_t kept = 0;
  if constexpr(injected_group)
  {
    if(not injecting() and (triggered() or std::exchange(state_.continued, false)))
    {
      kept = state_.kept_held;
    }
  }
  auto const clock = source();
  state_.clock_hz  = clock.hz;
  state_.divider   = clock.divider;
  state_.start_ps  = start_ps;
  state_.start_ps -= after(std::uint64_t{kept} * conversion()) - start_ps;
  state_.held = kept;
  state_.sum  = 0;
  if constexpr(injected_group)
  {
    if(kept != 0)
    {
      state_.sum = state_.kept_sum;
    }
  }
  state_.waiting_clock = false;
  state_.converting    = true;
  ++state_.generation;
  ctx_.events.schedule_at(
      vtime{after(std::uint64_t{due_samples()} * conversion())}, conversion_end, state_.generation,
      ctx_.clock);
  if(get(regs_, end_of_sampling_) == 0 and not injecting())
  {
    schedule_sampling_end(start_ps);
  }
}

template <typename Traits>
void adc_model<Traits>::schedule_sampling_end(std::uint64_t not_before_ps)
{
  for(std::uint32_t k = 0, samples = ratio(); k < samples; ++k)
  {
    auto const at = after(std::uint64_t{k} * conversion() + sampling());
    if(at > not_before_ps)
    {
      ctx_.events.schedule_at(vtime{at}, sampling_end, state_.generation, ctx_.clock);
      return;
    }
  }
}

template <typename Traits> void adc_model<Traits>::finish()
{
  if constexpr(injected_group)
  {
    if(state_.injecting)
    {
      finish_injected();
      return;
    }
  }
  auto const due    = due_samples();
  auto const end    = after(std::uint64_t{due} * conversion());
  state_.converting = false;
  for(; state_.held < due; ++state_.held)
  {
    state_.sum += code();
  }
  if constexpr(Traits::triggered_ovs)
  {
    if(triggered() and state_.held < ratio())
    {
      state_.kept_held = state_.held;
      state_.kept_sum  = state_.sum;
      update(regs_, start_, get(regs_, trigger_) != 0 ? 1u : 0u);
      signal();
      return;
    }
    state_.kept_held = 0;
    state_.kept_sum  = 0;
  }
  auto const result = oversampled() ? shifted(state_.sum) : state_.sum;
  watch(channel(), result);
  land(aligned(channel(), result));
  auto const last = std::size_t{state_.rank} + 1u >= length();
  if(last)
  {
    update(regs_, end_of_sequence_, 1);
  }
  advance(end, last);
  signal();
}

template <typename Traits> void adc_model<Traits>::advance(std::uint64_t end_ps, bool last)
{
  state_.rank      = last ? std::uint8_t{0} : static_cast<std::uint8_t>(state_.rank + 1u);
  auto const stays = get(regs_, trigger_) != 0 ? 1u : 0u;
  if constexpr(injected_group)
  {
    if(last and automatic())
    {
      if(get(regs_, continuous_) == 0)
      {
        update(regs_, start_, stays);
      }
      else
      {
        state_.regular_pending = true;
      }
      begin_injected(end_ps);
      return;
    }
  }
  if(regular_discontinuous())
  {
    if(last or state_.subgroup_left <= 1)
    {
      state_.subgroup_left = 0;
      update(regs_, start_, stays);
      return;
    }
    --state_.subgroup_left;
    begin_rank(end_ps);
    return;
  }
  if(last and get(regs_, continuous_) == 0)
  {
    update(regs_, start_, stays);
    return;
  }
  begin_rank(end_ps);
}

template <typename Traits>
auto adc_model<Traits>::guarded(std::size_t watchdog, std::uint32_t channel) const -> bool
{
  if(watchdog == 0)
  {
    auto enabled = watchdog1_;
    if constexpr(injected_group)
    {
      enabled = state_.injecting ? injected_->watchdog1 : watchdog1_;
    }
    return get(regs_, enabled) != 0
       and (get(regs_, watchdog1_single_) == 0 or get(regs_, watchdog1_channel_) == channel);
  }
  return channel < channels and (get(regs_, watchdog_channels_[watchdog]) >> channel & 1u) != 0;
}

template <typename Traits>
void adc_model<Traits>::watch(std::uint32_t channel, std::uint32_t result)
{
  for(std::size_t watchdog = 0; watchdog < watchdogs; ++watchdog)
  {
    if(not guarded(watchdog, channel))
    {
      continue;
    }
    auto const compared =
        Traits::compared(watchdog, result, get(regs_, resolution_), result_oversampled());
    auto out = compared < get(regs_, watchdog_low_[watchdog])
            or compared > get(regs_, watchdog_high_[watchdog]);
    if(watchdog == 0)
    {
      state_.filter_count =
          out ? std::min<std::uint8_t>(state_.filter_count + 1u, max_filter_count) : 0;
      out = state_.filter_count > (watchdog1_filter_ ? get(regs_, *watchdog1_filter_) : 0u);
    }
    if(out)
    {
      update(regs_, watchdog_flags_[watchdog], 1);
    }
  }
}

template <typename Traits>
auto adc_model<Traits>::aligned(std::uint32_t channel, std::uint32_t result) const -> std::uint32_t
{
  if constexpr(Traits::subtracts_offset)
  {
    if(not alignment_ignored())
    {
      auto const applied = std::ranges::find_if(offsets_, [&](offset_fields const& offset) {
        return get(regs_, *offset.enable) != 0 and get(regs_, offset.channel) == channel;
      });
      if(applied != offsets_.end())
      {
        return Traits::offset_result(
            result, get(regs_, applied->value), get(regs_, resolution_), get(regs_, *align_) != 0);
      }
    }
  }
  if constexpr(not Traits::align.empty())
  {
    if(get(regs_, *align_) != 0 and not alignment_ignored())
    {
      return Traits::left_aligned(result, get(regs_, resolution_));
    }
  }
  return result;
}

template <typename Traits> void adc_model<Traits>::land(std::uint32_t result)
{
  if constexpr(Traits::dma_end_freezes)
  {
    if(state_.dma_stopped)
    {
      return;
    }
  }
  if(get(regs_, overrun_mode_) == 0)
  {
    auto const overrun = get(regs_, overrun_) != 0;
    if(Traits::overrun_flush and not overrun and state_.data.size() >= Traits::result_depth)
    {
      update(regs_, overrun_, 1);
      state_.data.resize(1);
      return;
    }
    auto const lands = Traits::overrun_flush and overrun and state_.data.empty();
    if(not lands and (overrun or state_.data.size() >= Traits::result_depth))
    {
      update(regs_, overrun_, 1);
      return;
    }
  }
  else
  {
    if(get(regs_, end_of_conversion_) != 0)
    {
      update(regs_, overrun_, 1);
    }
    state_.data.clear();
  }
  state_.data.push_back(result);
  update(regs_, data_, state_.data.front());
  update(regs_, end_of_conversion_, 1);
}

template <typename Traits> void adc_model<Traits>::pop()
{
  if(not state_.data.empty())
  {
    state_.data.erase(state_.data.begin());
  }
  if(not state_.data.empty())
  {
    update(regs_, data_, state_.data.front());
  }
  update(regs_, end_of_conversion_, state_.data.empty() ? 0u : 1u);
  signal();
}

template <typename Traits> void adc_model<Traits>::abort()
{
  if constexpr(injected_group)
  {
    state_.regular_resume  = false;
    state_.regular_pending = false;
    state_.continued       = false;
    state_.kept_held       = 0;
    state_.kept_sum        = 0;
  }
  if(injecting())
  {
    state_.rank          = 0;
    state_.subgroup_left = 0;
    return;
  }
  ++state_.generation;
  state_.converting    = false;
  state_.waiting_clock = false;
  state_.rank          = 0;
  state_.subgroup_left = 0;
}

template <typename Traits> auto adc_model<Traits>::dma() const -> dma_mode
{
  std::array<std::uint32_t, Traits::dma_fields.size()> raw{};
  std::ranges::transform(
      dma_mode_, raw.begin(), [this](field_ref bits) { return get(regs_, bits); });
  return Traits::decode_dma(raw);
}

template <typename Traits> auto adc_model<Traits>::dma_level() const -> bool
{
  auto const mode = dma();
  auto const enabled =
      mode == dma_mode::circular or (mode == dma_mode::one_shot and not state_.dma_stopped);
  return enabled and not state_.data.empty() and get(regs_, overrun_) == 0;
}

template <typename Traits> void adc_model<Traits>::signal()
{
  auto const level = std::ranges::any_of(interrupts_, [this](auto const& pair) {
    return get(regs_, pair.first) != 0 and get(regs_, pair.second) != 0;
  });
  if(level)
  {
    line_.raise();
  }
  else
  {
    line_.lower();
  }
  request_.set(dma_level());
}

template <typename Traits> auto adc_model<Traits>::listened() const -> std::optional<std::uint32_t>
{
  if(get(regs_, enable_) == 0 or get(regs_, start_) == 0 or get(regs_, trigger_) == 0)
  {
    return std::nullopt;
  }
  return get(regs_, trigger_select_);
}

template <typename Traits>
auto adc_model<Traits>::regular_listens(std::string_view signal) const -> bool
{
  auto const selected = listened();
  return selected and trigger_signals<typename Traits::trigger_enum>()[*selected & 31u] == signal;
}

template <typename Traits> auto adc_model<Traits>::listens(std::string_view signal) const -> bool
{
  return regular_listens(signal) or injected_listens(signal);
}

template <typename Traits> auto adc_model<Traits>::can_listen(std::string_view signal) const -> bool
{
  if constexpr(injected_group)
  {
    if(std::ranges::contains(trigger_signals<typename Traits::injected_enum>(), signal))
    {
      return true;
    }
  }
  return std::ranges::contains(trigger_signals<typename Traits::trigger_enum>(), signal);
}

template <typename Traits>
void adc_model<Traits>::triggered(std::string_view signal, trigger_edge edge, trigger_time at)
{
  if constexpr(injected_group)
  {
    injected_triggered(signal, edge, at);
  }
  auto const polarity = get(regs_, trigger_);
  auto const detected = edge == trigger_edge::rising ? (polarity & exten_rising) != 0
                                                     : (polarity & exten_falling) != 0;
  if(not detected or not regular_listens(signal))
  {
    return;
  }
  auto const free = std::ranges::find(state_.edges, false, &adc_edge::pending);
  if(at.ps > ctx_.clock.time().ps and free != state_.edges.end())
  {
    *free = {
        .pending    = true,
        .falling    = edge == trigger_edge::falling,
        .reposted   = false,
        .selection  = static_cast<std::uint8_t>(*listened()),
        .generation = ++state_.edge_generation,
        .ps         = at.ps};
    ctx_.events.schedule_at(vtime{at.ps}, edge_due, free->generation, ctx_.clock);
    return;
  }
  if(busy())
  {
    latch_regular();
    return;
  }
  begin_sequence(at.ps + trigger_latency_ps(at.ps));
}

template <typename Traits> void adc_model<Traits>::fire_edge(adc_edge& slot)
{
  auto const polarity  = get(regs_, trigger_);
  auto const detected  = (polarity & (slot.falling ? exten_falling : exten_rising)) != 0;
  auto const selection = listened();
  if(not detected or selection != std::optional<std::uint32_t>{slot.selection})
  {
    slot.pending = false;
    return;
  }
  if(busy())
  {
    if(not std::exchange(slot.reposted, true))
    {
      ctx_.events.schedule_at(vtime{slot.ps}, edge_due, slot.generation, ctx_.clock);
      return;
    }
    slot.pending = false;
    latch_regular();
    return;
  }
  slot.pending = false;
  begin_sequence(slot.ps + trigger_latency_ps(slot.ps));
}

template <typename Traits>
auto adc_model<Traits>::trigger_latency_ps(std::uint64_t at_ps, bool aborting) -> std::uint64_t
{
  auto const clock = source();
  if(clock.hz == 0)
  {
    return 0;
  }
  auto const* const shared = common();
  auto const        mode   = shared != nullptr ? shared->clock_mode() : 0u;
  auto              edge   = std::uint64_t{};
  if(Traits::trigger_waits_edge and mode == 0)
  {
    auto const period = wide{clock.divider} * ps_per_second;
    auto const index  = (wide{at_ps} * clock.hz + period - 1u) / period;
    edge = static_cast<std::uint64_t>((index * period + clock.hz - 1u) / clock.hz) - at_ps;
  }
  auto eighths = Traits::trigger_latency_eighths(clock.divider, mode);
  if constexpr(injected_group)
  {
    if(aborting)
    {
      eighths = Traits::injected_abort_latency_eighths(mode);
    }
  }
  return edge + cycles_ps(eighths, 8, clock.divider, clock.hz);
}

template <typename Traits>
auto adc_model<Traits>::on_write(
    register_file& regs,
    std::uint32_t  offset,
    std::uint8_t   size,
    std::uint32_t  old,
    std::uint32_t  value,
    access_ctx     access) -> std::expected<void, bus_error>
{
  if constexpr(injected_group)
  {
    auto const word = offset & ~3u;
    if(word == start_.offset or word == injected_->sequence_register.offset)
    {
      if(word != start_.offset
         and get(regs_, injected_start_) != 0
         and get(regs_, injected_->queue_disable) != 0)
      {
        forbidden(
            sequence_written_logged_, "ADC_JSQR",
            "ADC_JSQR written with JADSTART 1 and JQDIS 1, RM0351 18.7.16 as corrected by ES0250 "
            "2.8.2: the write applies to the next injected sequence");
      }
      auto const queued = word != start_.offset and queue_on() and access.by != initiator::debugger;
      auto const held   = (state_.queue_pending ? 1u : 0u) + state_.queue_count;
      if(queued and held >= (emptying() ? 2u : 1u))
      {
        update(regs_, injected_->queue_overflow, 1);
        signal();
        return {};
      }
      auto const behind =
          emptying() ? not state_.queue_empty
                     : injected_discontinuous() and (state_.injecting or state_.injected_rank != 0);
      if(queued and behind)
      {
        state_.queue_next[state_.queue_count++] = merge_write(old, offset, size, value);
        return {};
      }
      auto const was = injected_listened();
      injected_notify(was);
      if(queued)
      {
        state_.queue_empty = false;
      }
      auto const result = write_word(regs, offset, size, old, value, access);
      retune_injected(was);
      if(queued)
      {
        state_.queue_pending = true;
      }
      return result;
    }
    if(word == trigger_.offset)
    {
      auto const before = get(regs_, injected_->queue_disable);
      auto const result = write_word(regs, offset, size, old, value, access);
      if(get(regs_, injected_->queue_disable) != before)
      {
        flush_queue();
        apply_context(describe("ADC_JSQR").reset, true);
      }
      return result;
    }
  }
  return write_word(regs, offset, size, old, value, access);
}

template <typename Traits>
auto adc_model<Traits>::write_word(
    register_file& regs,
    std::uint32_t  offset,
    std::uint8_t   size,
    std::uint32_t  old,
    std::uint32_t  value,
    access_ctx     access) -> std::expected<void, bus_error>
{
  auto const word = offset & ~3u;
  if(word != start_.offset and word != trigger_.offset)
  {
    return base::on_write(regs, offset, size, old, value, access);
  }
  auto const config   = regs_.peek(trigger_.offset);
  auto const next     = word == trigger_.offset ? merge_write(config, offset, size, value) : config;
  auto const external = trigger_.of(config) != 0;
  auto const moved    = trigger_select_.of(next) != trigger_select_.of(config);
  if(external)
  {
    static_cast<void>(interest_changed(
        ctx_.peers, trigger_signal<typename Traits::trigger_enum>(trigger_select_.of(config))));
  }
  if(trigger_.of(next) != 0 and (not external or moved))
  {
    static_cast<void>(interest_changed(
        ctx_.peers, trigger_signal<typename Traits::trigger_enum>(trigger_select_.of(next))));
  }
  auto const was    = listened();
  auto const result = base::on_write(regs, offset, size, old, value, access);
  retune(was);
  return result;
}

template <typename Traits> void adc_model<Traits>::retune(std::optional<std::uint32_t> was)
{
  auto const now = listened();
  if(now == was)
  {
    return;
  }
  if(was)
  {
    static_cast<void>(
        interest_changed(ctx_.peers, trigger_signal<typename Traits::trigger_enum>(*was)));
  }
  if(not now)
  {
    return;
  }
  auto const signal = trigger_name<typename Traits::trigger_enum>(*now);
  if(not interest_changed(ctx_.peers, trigger_signal<typename Traits::trigger_enum>(*now)))
  {
    unmodelled(
        unproduced_logged_[*now], Traits::config,
        signal.starts_with("adc_ext_trg")
            ? std::format("EXTSEL {}: reserved trigger selection", *now)
            : std::format("EXTSEL {}: trigger {}", *now, signal));
  }
}

template <typename Traits> auto adc_model<Traits>::injecting() const -> bool
{
  if constexpr(injected_group)
  {
    return state_.injecting;
  }
  return false;
}

template <typename Traits> auto adc_model<Traits>::injected_length() const -> std::size_t
{
  return get(regs_, injected_->length) + std::size_t{1};
}

template <typename Traits>
auto adc_model<Traits>::injected_listened() const -> std::optional<std::uint32_t>
{
  if constexpr(injected_group)
  {
    if(get(regs_, enable_) != 0
       and get(regs_, injected_start_) != 0
       and get(regs_, injected_->trigger) != 0
       and not(emptying() and state_.queue_empty))
    {
      return get(regs_, injected_->select);
    }
  }
  return std::nullopt;
}

template <typename Traits>
auto adc_model<Traits>::injected_listens(std::string_view signal) const -> bool
{
  if constexpr(injected_group)
  {
    auto const selected = injected_listened();
    return selected
       and trigger_signals<typename Traits::injected_enum>()[*selected & 31u] == signal;
  }
  return false;
}

template <typename Traits>
void adc_model<Traits>::injected_notify(std::optional<std::uint32_t> selection)
{
  if constexpr(injected_group)
  {
    if(selection)
    {
      static_cast<void>(
          interest_changed(ctx_.peers, trigger_signal<typename Traits::injected_enum>(*selection)));
    }
  }
}

template <typename Traits> void adc_model<Traits>::retune_injected(std::optional<std::uint32_t> was)
{
  if constexpr(injected_group)
  {
    auto const now = injected_listened();
    if(now == was)
    {
      return;
    }
    injected_notify(was);
    if(not now)
    {
      return;
    }
    if(not interest_changed(ctx_.peers, trigger_signal<typename Traits::injected_enum>(*now)))
    {
      unmodelled(
          injected_unproduced_logged_[*now], "ADC_JSQR",
          std::format(
              "JEXTSEL {}: trigger {}", *now, trigger_name<typename Traits::injected_enum>(*now)));
    }
  }
}

template <typename Traits>
void adc_model<Traits>::injected_triggered(
    std::string_view signal,
    trigger_edge     edge,
    trigger_time     at)
{
  if constexpr(injected_group)
  {
    auto const polarity = get(regs_, injected_->trigger);
    auto const detected = edge == trigger_edge::rising ? (polarity & exten_rising) != 0
                                                       : (polarity & exten_falling) != 0;
    if(not detected or not injected_listens(signal) or automatic())
    {
      return;
    }
    auto const free = std::ranges::find(state_.injected_edges, false, &adc_edge::pending);
    if(at.ps > ctx_.clock.time().ps and free != state_.injected_edges.end())
    {
      *free = {
          .pending    = true,
          .falling    = edge == trigger_edge::falling,
          .reposted   = false,
          .selection  = static_cast<std::uint8_t>(*injected_listened()),
          .generation = ++state_.injected_edge_generation,
          .ps         = at.ps};
      ctx_.events.schedule_at(vtime{at.ps}, injected_edge_due, free->generation, ctx_.clock);
      return;
    }
    start_triggered_injected(at.ps);
  }
}

template <typename Traits> void adc_model<Traits>::fire_injected(adc_edge& slot)
{
  if constexpr(injected_group)
  {
    auto const polarity  = get(regs_, injected_->trigger);
    auto const detected  = (polarity & (slot.falling ? exten_falling : exten_rising)) != 0;
    auto const selection = injected_listened();
    if(not detected or selection != std::optional<std::uint32_t>{slot.selection})
    {
      slot.pending = false;
      return;
    }
    if(busy() and not std::exchange(slot.reposted, true))
    {
      ctx_.events.schedule_at(vtime{slot.ps}, injected_edge_due, slot.generation, ctx_.clock);
      return;
    }
    slot.pending = false;
    start_triggered_injected(slot.ps);
  }
}

template <typename Traits> void adc_model<Traits>::start_triggered_injected(std::uint64_t at_ps)
{
  if(injecting())
  {
    return;
  }
  if(busy())
  {
    preempt(at_ps);
    return;
  }
  begin_injected(at_ps + trigger_latency_ps(at_ps));
}

template <typename Traits>
void adc_model<Traits>::injected_written(std::uint32_t old, std::uint32_t now)
{
  if constexpr(injected_group)
  {
    if(injected_->stop.of(now) != 0)
    {
      update(regs_, injected_->stop, 0);
      update(regs_, injected_start_, 0);
      abort_injected();
      flush_queue();
      resume_regular(ctx_.clock.time().ps);
      signal();
      return;
    }
    if(injected_start_.of(now) == 0 or injected_start_.of(old) != 0)
    {
      return;
    }
    try
    {
      prepare_injected();
      if(get(regs_, injected_->trigger) != 0)
      {
        return;
      }
      if(busy())
      {
        preempt(ctx_.clock.time().ps);
        return;
      }
    }
    catch(...)
    {
      regs_.poke(enable_.offset, old);
      throw;
    }
    begin_injected(ctx_.clock.time().ps);
  }
}

template <typename Traits> void adc_model<Traits>::prepare_injected()
{
  if constexpr(injected_group)
  {
    if constexpr(Traits::injected_ovs)
    {
      check_oversampling_modes();
    }
    else if(injected_oversampled())
    {
      unmodelled(injected_oversampling_logged_, "ADC_CFGR2", "JOVSE: injected oversampling");
    }
    for(std::size_t rank = 0; rank < injected_length(); ++rank)
    {
      auto const channel = get(regs_, injected_->sequence[rank]);
      check_unmodelled(channel);
      check_analog_switch(channel);
      require_factory_word(channel);
    }
  }
}

template <typename Traits> void adc_model<Traits>::begin_injected(std::uint64_t start_ps)
{
  if constexpr(injected_group)
  {
    if(not injected_discontinuous() or state_.injected_rank >= injected_length())
    {
      state_.injected_rank = 0;
    }
    if(queue_on() and injected_discontinuous() and state_.injected_rank == 0)
    {
      state_.queue_pending = false;
    }
    if(triggered())
    {
      state_.kept_held = 0;
      state_.kept_sum  = 0;
    }
    state_.injecting = true;
    begin_rank(start_ps);
  }
}

template <typename Traits> void adc_model<Traits>::finish_injected()
{
  if constexpr(injected_group)
  {
    auto const end    = after(std::uint64_t{ratio()} * conversion());
    state_.converting = false;
    for(auto const samples = ratio(); state_.held < samples; ++state_.held)
    {
      state_.sum += code();
    }
    auto const result = result_oversampled() ? shifted(state_.sum) : state_.sum;
    auto const rank   = std::size_t{state_.injected_rank};
    watch(channel(), result);
    update(regs_, injected_->data[rank], aligned(channel(), result));
    update(regs_, injected_->end_of_conversion, 1);
    auto const last     = rank + 1u >= injected_length();
    auto const software = get(regs_, injected_->trigger) == 0;
    if(last)
    {
      update(regs_, injected_->end_of_sequence, 1);
      advance_queue();
    }
    state_.injected_rank = last ? std::uint8_t{0} : static_cast<std::uint8_t>(rank + 1u);
    if(last or injected_discontinuous())
    {
      state_.injecting = false;
      if(software)
      {
        update(regs_, injected_start_, 0);
      }
      resume_regular(end);
    }
    else
    {
      begin_rank(end);
    }
    signal();
  }
}

template <typename Traits> void adc_model<Traits>::abort_injected()
{
  if constexpr(injected_group)
  {
    if(state_.injecting)
    {
      ++state_.generation;
      state_.converting    = false;
      state_.waiting_clock = false;
    }
    state_.injecting     = false;
    state_.injected_rank = 0;
  }
}

template <typename Traits> void adc_model<Traits>::latch_regular()
{
  if constexpr(injected_group)
  {
    if(state_.injecting)
    {
      state_.regular_pending = true;
    }
  }
}

template <typename Traits> auto adc_model<Traits>::automatic() const -> bool
{
  if constexpr(injected_group)
  {
    return get(regs_, injected_->automatic) != 0;
  }
  return false;
}

template <typename Traits> auto adc_model<Traits>::queue_on() const -> bool
{
  if constexpr(injected_group)
  {
    return get(regs_, injected_->queue_disable) == 0;
  }
  return false;
}

template <typename Traits> auto adc_model<Traits>::emptying() const -> bool
{
  if constexpr(injected_group)
  {
    return queue_on() and get(regs_, injected_->queue_mode) != 0;
  }
  return false;
}

template <typename Traits> void adc_model<Traits>::apply_context(std::uint32_t word, bool empty)
{
  if constexpr(injected_group)
  {
    auto const was = injected_listened();
    injected_notify(was);
    regs_.poke(injected_->sequence_register.offset, word);
    state_.queue_empty = empty;
    retune_injected(was);
  }
}

template <typename Traits> void adc_model<Traits>::advance_queue()
{
  if constexpr(injected_group)
  {
    state_.queue_pending = false;
    if(state_.queue_count == 0)
    {
      if(emptying())
      {
        flush_queue();
      }
      return;
    }
    auto const next      = state_.queue_next[0];
    state_.queue_next[0] = state_.queue_next[1];
    --state_.queue_count;
    state_.queue_pending = true;
    apply_context(next);
  }
}

template <typename Traits> void adc_model<Traits>::flush_queue()
{
  if constexpr(injected_group)
  {
    state_.queue_pending = false;
    state_.queue_count   = 0;
    if(not emptying())
    {
      return;
    }
    apply_context(0, true);
  }
}

template <typename Traits> auto adc_model<Traits>::regular_discontinuous() const -> bool
{
  return (get(regs_, discontinuous_) != 0 or triggered()) and not automatic();
}

template <typename Traits> auto adc_model<Traits>::injected_discontinuous() const -> bool
{
  if constexpr(injected_group)
  {
    return get(regs_, injected_->discontinuous) != 0 and not automatic();
  }
  return false;
}

template <typename Traits> void adc_model<Traits>::check_automatic()
{
  if constexpr(injected_group)
  {
    if(get(regs_, injected_->trigger) != 0)
    {
      forbidden(
          automatic_trigger_logged_, Traits::config,
          "JAUTO with JEXTEN set, forbidden by RM0351 18.4.19: the injected triggers are "
          "ignored, converted as JAUTO alone");
    }
    if(oversampled() and injected_oversampling_ and get(regs_, *injected_oversampling_) == 0)
    {
      forbidden(
          automatic_oversampling_logged_, Traits::config,
          "JAUTO with ROVSE 1 and JOVSE 0, not supported by RM0351 18.4.30: the regular ranks "
          "are oversampled, the injected ranks converted once");
    }
    if(get(regs_, discontinuous_) != 0 or get(regs_, injected_->discontinuous) != 0)
    {
      forbidden(
          automatic_discontinuous_logged_, Traits::config,
          "JAUTO with DISCEN or JDISCEN, forbidden by RM0351 18.4.19 and 18.7.4: converted as "
          "JAUTO alone");
    }
  }
}

template <typename Traits> void adc_model<Traits>::preempt(std::uint64_t at_ps)
{
  if constexpr(injected_group)
  {
    keep_samples(at_ps);
    ++state_.generation;
    state_.converting     = false;
    state_.waiting_clock  = false;
    state_.regular_resume = true;
    begin_injected(at_ps + trigger_latency_ps(at_ps, true));
  }
}

template <typename Traits> void adc_model<Traits>::keep_samples(std::uint64_t at_ps)
{
  if constexpr(injected_group)
  {
    state_.continued = state_.converting
                   and oversampled()
                   and get(regs_, injected_->resumed_oversampling) == 0
                   and not injected_oversampled()
                   and not triggered();
    if(not state_.continued)
    {
      return;
    }
    hold_until(at_ps);
    auto completed = std::uint32_t{};
    while(completed < state_.held and after(std::uint64_t{completed + 1u} * conversion()) <= at_ps)
    {
      ++completed;
    }
    if(state_.held > completed)
    {
      state_.sum -= state_.last_code;
      state_.held = completed;
    }
    state_.kept_held = state_.held;
    state_.kept_sum  = state_.sum;
  }
}

template <typename Traits> void adc_model<Traits>::resume_regular(std::uint64_t at_ps)
{
  if constexpr(injected_group)
  {
    if(std::exchange(state_.regular_resume, false))
    {
      state_.regular_pending = false;
      begin_rank(at_ps);
      return;
    }
    if(std::exchange(state_.regular_pending, false)
       and get(regs_, enable_) != 0
       and get(regs_, start_) != 0)
    {
      begin_sequence(at_ps);
    }
  }
}

template <typename Traits>
void adc_model<Traits>::transfer_completed(dma_controller const& by, std::uint32_t request)
{
  if(dma() != dma_mode::one_shot or not request_.carries(by, request))
  {
    return;
  }
  state_.dma_stopped = true;
  if(automatic())
  {
    abort_injected();
  }
  abort();
  if(Traits::dma_end_stops or automatic())
  {
    auto const was = listened();
    if(was)
    {
      static_cast<void>(
          interest_changed(ctx_.peers, trigger_signal<typename Traits::trigger_enum>(*was)));
    }
    update(regs_, start_, 0);
    retune(was);
  }
  signal();
}

template <typename Traits>
void adc_model<Traits>::on_event(std::uint32_t kind, std::uint64_t payload)
{
  if(kind == ready_end)
  {
    if(payload == state_.ready_generation and get(regs_, enable_) != 0)
    {
      update(regs_, ready_, 1);
      signal();
    }
    return;
  }
  if constexpr(Traits::calibration_cycles.has_value())
  {
    if(kind == calibration_end)
    {
      if(payload == state_.calibration_generation and get(regs_, calibrate_) != 0)
      {
        end_calibration();
      }
      return;
    }
  }
  if constexpr(injected_group)
  {
    if(kind == injected_edge_due)
    {
      auto const found =
          std::ranges::find_if(state_.injected_edges, [payload](adc_edge const& slot) {
            return slot.pending and slot.generation == payload;
          });
      if(found != state_.injected_edges.end())
      {
        fire_injected(*found);
      }
      return;
    }
  }
  if(kind == edge_due)
  {
    auto const found = std::ranges::find_if(state_.edges, [payload](adc_edge const& slot) {
      return slot.pending and slot.generation == payload;
    });
    if(found != state_.edges.end())
    {
      fire_edge(*found);
    }
    return;
  }
  if(payload != state_.generation or not state_.converting)
  {
    return;
  }
  if(kind == sampling_end)
  {
    update(regs_, end_of_sampling_, 1);
    signal();
    return;
  }
  if(kind == conversion_end)
  {
    finish();
  }
}

template <typename Traits> void adc_model<Traits>::on_clock_change()
{
  if constexpr(Traits::calibration_cycles.has_value())
  {
    if(state_.calibration_waiting and source().hz != 0)
    {
      start_calibration();
    }
  }
  if(state_.waiting_clock and source().hz != 0)
  {
    schedule(ctx_.clock.time().ps);
  }
}

template <typename Traits> void adc_model<Traits>::require_factory_word(std::uint32_t channel) const
{
  auto const* const shared = common();
  if(shared == nullptr)
  {
    return;
  }
  if(channel == inputs_.vrefint and shared->vrefint_connected())
  {
    static_cast<void>(calibration(Traits::vrefint_cal_address, "VREFINT_CAL"));
  }
  if(channel == inputs_.sensor and shared->sensor_connected())
  {
    static_cast<void>(calibration(Traits::ts_cal1_address, "TS_CAL1"));
  }
}

template <typename Traits>
auto adc_model<Traits>::sample(std::uint32_t channel) const -> std::uint32_t
{
  auto const* const shared = common();
  if(channel == inputs_.vrefint)
  {
    return shared != nullptr and shared->vrefint_connected()
             ? at_vdda<Traits>(
                   calibration(Traits::vrefint_cal_address, "VREFINT_CAL"), supply_.vdda_mv)
             : 0u;
  }
  if(channel == inputs_.sensor)
  {
    return shared != nullptr and shared->sensor_connected()
             ? at_vdda<Traits>(calibration(Traits::ts_cal1_address, "TS_CAL1"), supply_.vdda_mv)
             : 0u;
  }
  if(is_external(channel) and not option_open(channel) and not switch_open(channel))
  {
    return code_of<Traits>(level_uv(channel), supply_.vdda_mv);
  }
  return 0;
}

template <typename Traits>
void adc_model<Traits>::warn_once(bool& logged, std::string_view reg, std::string_view what)
{
  if(std::exchange(logged, true))
  {
    return;
  }
  warn(reg, std::format("{} not modelled", what));
}

template <typename Traits>
void adc_model<Traits>::forbidden(bool& logged, std::string_view name, std::string message)
{
  this->warn_once_or_refuse(
      logged, name, ctx_.strict_peripherals ? reg(name).offset : 0u, std::move(message));
}

template <typename Traits>
void adc_model<Traits>::unmodelled(bool& logged, std::string_view name, std::string_view what)
{
  this->warn_once_or_refuse(logged, name, ctx_.strict_peripherals ? reg(name).offset : 0u, [&] {
    return std::format("{} not modelled", what);
  });
}

template <typename Traits> auto adc_common_model<Traits>::status() const -> std::uint32_t
{
  auto const flags_of = [this](std::string_view instance) {
    auto const* const unit = dynamic_cast<adc_model<Traits> const*>(ctx_.peers.find(instance));
    return unit != nullptr ? unit->flags() & Traits::status_flags : 0u;
  };
  return flags_of(Traits::status_master)
       | flags_of(Traits::status_slave) << Traits::status_slave_shift;
}

template <typename Traits>
auto adc_common_model<Traits>::on_read(
    register_file& regs,
    std::uint32_t  offset,
    std::uint8_t   size,
    access_ctx     ctx) -> std::expected<std::uint32_t, bus_error>
{
  if constexpr(not Traits::common_status.empty())
  {
    if((offset & ~3u) == status_->offset)
    {
      regs_.poke(status_->offset, status());
    }
  }
  return base::on_read(regs, offset, size, ctx);
}
}

auto adc_factory(supply_options supply) -> model_factory
{
  return [supply](model_context& ctx, peripheral_desc const& desc) -> std::shared_ptr<model> {
    return std::make_shared<adc_model<u5_adc>>(ctx, desc, supply);
  };
}

auto adc_common_factory() -> model_factory
{
  return [](model_context& ctx, peripheral_desc const& desc) -> std::shared_ptr<model> {
    return std::make_shared<adc_common_model<u5_adc>>(ctx, desc);
  };
}

auto adc_h5_factory(supply_options supply) -> model_factory
{
  return [supply](model_context& ctx, peripheral_desc const& desc) -> std::shared_ptr<model> {
    return std::make_shared<adc_model<h5_adc>>(ctx, desc, supply);
  };
}

auto adc_common_h5_factory() -> model_factory
{
  return [](model_context& ctx, peripheral_desc const& desc) -> std::shared_ptr<model> {
    return std::make_shared<adc_common_model<h5_adc>>(ctx, desc);
  };
}

auto adc_l4_factory(supply_options supply) -> model_factory
{
  return [supply](model_context& ctx, peripheral_desc const& desc) -> std::shared_ptr<model> {
    return std::make_shared<adc_model<l4_adc>>(ctx, desc, supply);
  };
}

auto adc_common_l4_factory() -> model_factory
{
  return [](model_context& ctx, peripheral_desc const& desc) -> std::shared_ptr<model> {
    return std::make_shared<adc_common_model<l4_adc>>(ctx, desc);
  };
}
}
