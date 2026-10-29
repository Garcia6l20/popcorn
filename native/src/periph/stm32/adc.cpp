#include "dma.hpp"
#include "dma_signal.hpp"
#include "factories.hpp"
#include "model_base.hpp"
#include "trigger_signal.hpp"

#include <popcorn/core/clock.hpp>
#include <popcorn/periph/stm32/analog.hpp>
#include <popcorn/periph/stm32/supply.hpp>

#include <reflex/enum.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <optional>
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
constexpr std::uint64_t ps_per_second      = 1'000'000'000'000;
constexpr std::uint32_t conversion_end     = 0;
constexpr std::uint32_t sampling_end       = 1;
constexpr std::uint32_t ready_end          = 2;
constexpr std::size_t   trigger_selections = 32;
constexpr std::uint32_t exten_rising       = 1;
constexpr std::uint32_t exten_falling      = 2;
constexpr std::size_t   watchdogs          = 3;
constexpr std::uint8_t  max_filter_count   = 8;

using ready_pair = std::optional<std::array<std::uint32_t, 2>>;

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

struct adc_inputs
{
  std::string_view             instance;
  std::optional<std::uint32_t> vrefint;
  std::optional<std::uint32_t> sensor;
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
 *        EXTSEL map of Table 308, tLATR, the factory words and the eight-result FIFO.
 */
struct u5_adc
{
  using sequence     = rank_sequence;
  using trigger_enum = regular_trigger;

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
  static constexpr bool             trigger_waits_edge  = false;
  static constexpr ready_pair       ready_cycles        = std::nullopt;
  static constexpr std::uint32_t    result_mask         = 0xFFFF'FFFF;
  static constexpr unsigned         bits                = 14;
  static constexpr address          ts_cal1_address     = 0x0BFA'0710;
  static constexpr address          vrefint_cal_address = 0x0BFA'07A5;
  static constexpr std::uint32_t    factory_vdda_mv     = 3000;
  static constexpr std::string_view externals           = "from 1 to 17";
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
  static constexpr std::string_view injected_oversample = {};
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

  [[nodiscard]] static constexpr auto trigger_latency_quarters(std::uint32_t divider, std::uint32_t)
      -> std::uint32_t
  {
    return 4u * (divider == 1 ? 3u : 3u * divider + 1u);
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
  using sequence     = rank_sequence;
  using trigger_enum = h5_regular_trigger;

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
  static constexpr bool dma_end_stops = true;
  /** @brief A trigger at CKMODE 00 waits for the next ADC clock edge, measured on an STM32H573. */
  static constexpr bool trigger_waits_edge = true;
  /**
   * @brief HCLK cycles from the ADEN edge to ADRDY with ADVREGEN 1 and 0, measured on an
   *        STM32H573 with the ADC clock at HCLK.
   */
  static constexpr ready_pair       ready_cycles        = std::array<std::uint32_t, 2>{35, 45};
  static constexpr std::uint32_t    result_mask         = 0xFFFF;
  static constexpr unsigned         bits                = 12;
  static constexpr address          ts_cal1_address     = 0x08FF'F814;
  static constexpr address          vrefint_cal_address = 0x08FF'F810;
  static constexpr std::uint32_t    factory_vdda_mv     = 3300;
  static constexpr std::string_view externals           = "0 to 15, 18 or 19";
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
  static constexpr std::string_view injected_oversample = "JOVSE";
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
      trigger_latency_quarters(std::uint32_t, std::uint32_t clock_mode) -> std::uint32_t
  {
    constexpr std::array<std::uint32_t, 4> quarters{18, 10, 10, 9};
    return quarters[clock_mode & 3u];
  }
};

struct adc_state
{
  std::array<std::uint32_t, channels> input_uv{};
  std::uint32_t                       generation{};
  std::uint32_t                       ready_generation{};
  std::uint64_t                       start_ps{};
  std::uint64_t                       clock_hz{};
  std::uint32_t                       divider{};
  std::uint32_t                       held{};
  std::uint32_t                       sum{};
  bool                                converting{};
  bool                                waiting_clock{};
  std::uint8_t                        rank{};
  std::uint8_t                        subgroup_left{};
  std::vector<std::uint32_t>          data;
  bool                                dma_stopped{};
  std::uint8_t                        filter_count{};

  friend auto operator==(adc_state const&, adc_state const&) -> bool = default;
};

struct adc_common_state
{
  friend auto operator==(adc_common_state const&, adc_common_state const&) -> bool = default;
};

[[nodiscard]] auto factory_halfword(part_description const& part, address at)
    -> std::optional<std::uint16_t>
{
  for(auto const& region : part.regions)
  {
    if(not region.content)
    {
      continue;
    }
    for(auto const& piece : *region.content)
    {
      auto const first = region.start + piece.offset;
      if(at >= first and at + 2 <= first + piece.data.size())
      {
        auto const index = static_cast<std::size_t>(at - first);
        return static_cast<std::uint16_t>(piece.data[index] | (piece.data[index + 1] << 8));
      }
    }
  }
  return std::nullopt;
}

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

auto lower(std::string_view name) -> std::string
{
  std::string out{name};
  std::ranges::transform(
      out, out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
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
 *        and ADC1 listens, as a trigger_sink, to the signal EXTSEL selects in RM0456 Table 308,
 *        such as `tim15_trgo` for 14, while ADEN and ADSTART are set. EXTEN 01 detects the
 *        signal's rising edges, 10 its falling edges, 11 both: a timer's TRGO pulse rises at its
 *        event and falls one timer kernel clock later. Each detected edge while no conversion
 *        runs starts the sequence, or the next DISCEN subgroup, tLATR later: 3, 7 or 13 ADC
 *        clock cycles for PRESC 0, 1 or 2, datasheet DS13737 table 103. The datasheet gives no
 *        value for a larger PRESC: a divider D of 4 or more takes 3 x D + 1 cycles, the rule its
 *        values for D 2 and 4 follow, unverified. An edge during a conversion or the latency is
 *        ignored. ADSTART stays set at the end of a sequence or subgroup, RM0456
 *        33.4.16, and clears with ADSTP or ADDIS only. ADC1 tells the selected signal's source
 *        when its listening starts or stops. A selection no model produces, the reserved EXTSEL
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
 *        Results, RM0456 33.4.27: with OVRMOD 0, ADC_DR shows the head of an eight-result FIFO,
 *        a CPU or DMA read of ADC_DR pops it, and EOC stays set while results are left. A result
 *        finding the FIFO full sets OVR and is discarded, as is every result while OVR stays set.
 *        With OVRMOD 1, a result overwrites ADC_DR and sets OVR when EOC was still set. A
 *        debugger read pops nothing. DMNGT 01 and 11 hold the `adc1_dma` request high while a
 *        result is unread and OVR is clear. Under DMNGT 01, once the GPDMA channel serving that
 *        request completes, the conversion in progress is dropped, the sequence resets to rank
 *        0, ADSTART stays set, and no request rises until an ADC_CR write takes ADSTART from 0
 *        to 1: RM0456 ties the re-arm to nothing, this reading is unverified. A hardware trigger
 *        then starts sequences without a request, and their results land in ADC_DR: the data
 *        register RM0456 33.4.27 calls frozen after the DMA end is not modelled. DMNGT 10, the MDF
 *        path, requests nothing. ADC1_IRQn is high while an ADC_ISR flag is set with its ADC_IER
 *        enable.
 *
 *        Codes against VDDA, the board's `supply_options`: an external channel 1 to 17 reads
 *        its driven input, `analog_port`, as V x 16384 / VDDA, floored and saturated. VREFINT
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
 *        Injected conversions are not modelled and log a warning once.
 *        DMNGT 10, AUTDLY, LFTRIG, LSHIFT, offsets, gain compensation, BULB, SMPTRIG, TROVS and
 *        the DIFSEL bit of a converted channel leave the conversions unchanged and log a warning
 *        once, a model_error under strict_peripherals. Two settings
 *        RM0456 forbids log a warning once, a model_error under strict_peripherals: an ADSTART
 *        write with ADEN 0 or ADDIS 1, ignored with ADSTART left at 0, and DISCEN with CONT,
 *        converted as DISCEN alone. A sequence converting a channel whose ADC_PCSEL bit is
 *        clear logs a warning once, a model_error under strict_peripherals, and converts it as if
 *        the bit were set, RM0456 33.4.12. Only the single DMA request is modelled: the burst
 * request RM0456 33.4.27 raises at four results is not. A GPDMA channel reading ADC_DR in bursts of
 * more than one beat starts each burst on the single request, and a beat finding the FIFO empty
 * reads the last result again. A reserved ADC12_CCR.PRESC divides by 256, logged once by ADC12, a
 * model_error under strict_peripherals.
 *
 *        Analog watchdogs, RM0456 33.4.30: each regular conversion's result, after the
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
 */
template <typename Traits>
class adc_model final : public dispatch_model<adc_model<Traits>, adc_state>,
                        public analog_port,
                        public clock_listener,
                        public dma_requester,
                        public trigger_sink,
                        public bus_reset_listener
{
  using base = dispatch_model<adc_model<Traits>, adc_state>;
  using base::claims;
  using base::ctx_;
  using base::desc_;
  using base::describe;
  using base::field;
  using base::irq;
  using base::reg;
  using base::state_;
  using base::warn;

public:
  adc_model(model_context& ctx, peripheral_desc const& desc, supply_options supply);

  void               reset(register_file& regs, reset_kind kind) override;
  void               on_bus_reset() override;
  void               on_event(std::uint32_t kind, std::uint64_t payload) override;
  void               on_clock_change() override;
  void               transfer_completed(dma_controller const& by, std::uint32_t request) override;
  [[nodiscard]] auto listens(std::string_view signal) const -> bool override;
  void triggered(std::string_view signal, trigger_edge edge, std::uint64_t at_ps) override;

  auto on_read(register_file& regs, std::uint32_t offset, std::uint8_t size, access_ctx ctx)
      -> std::expected<std::uint32_t, bus_error> override;
  auto on_write(
      register_file& regs,
      std::uint32_t  offset,
      std::uint8_t   size,
      std::uint32_t  old,
      std::uint32_t  value,
      access_ctx     ctx) -> std::expected<void, bus_error> override;

  void               drive(std::uint8_t channel, std::uint32_t microvolts) override;
  [[nodiscard]] auto input(std::uint8_t channel) const -> std::uint32_t override;
  [[nodiscard]] auto external_channels() const -> std::uint32_t override;
  [[nodiscard]] auto bonded_channels() const -> std::uint32_t override
  {
    return bonded_channels_;
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
  [[nodiscard]] auto channel() const -> std::uint32_t;
  [[nodiscard]] auto channel_at(std::size_t rank) const -> std::uint32_t;
  [[nodiscard]] auto length() const -> std::size_t;
  [[nodiscard]] auto busy() const -> bool;
  [[nodiscard]] auto external(std::uint8_t channel) const -> std::size_t;
  [[nodiscard]] auto calibration(address at, std::string_view name) const -> std::uint16_t;
  [[nodiscard]] auto sample(std::uint32_t channel) const -> std::uint32_t;
  [[nodiscard]] auto dma() const -> dma_mode;
  [[nodiscard]] auto dma_level() const -> bool;
  [[nodiscard]] auto listened() const -> std::optional<std::uint32_t>;
  void               retune(std::optional<std::uint32_t> was);
  [[nodiscard]] auto trigger_latency_ps(std::uint64_t at_ps) -> std::uint64_t;
  void               require_factory_word(std::uint32_t channel) const;
  void               prepare();
  void               begin_sequence(std::uint64_t start_ps);
  void               begin_rank(std::uint64_t start_ps);
  void               schedule(std::uint64_t start_ps);
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
  void               check_preselection(std::uint32_t channel);
  [[nodiscard]] auto option_switch_set() const -> bool;
  void               check_option_switch(std::uint32_t channel);
  [[nodiscard]] auto option_open(std::uint32_t channel) const -> bool;
  void               warn_once(bool& logged, std::string_view reg, std::string_view what);
  void               unmodelled(bool& logged, std::string_view name, std::string_view what);
  void               forbidden(bool& logged, std::string_view name, std::string message);
  void               refuse_start(bool refused);

  register_file&                                   regs_;
  supply_options                                   supply_;
  std::uint32_t                                    bonded_channels_{};
  adc_inputs                                       inputs_;
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
  field_ref                                        bulb_;
  field_ref                                        sampling_trigger_;
  field_ref                                        triggered_oversampling_;
  field_ref                                        differential_;
  std::array<field_ref, channels>                  sampling_;
  std::array<offset_fields, offsets>               offsets_;
  std::optional<std::array<field_ref, channels>>   preselection_;
  field_ref                                        watchdog1_;
  field_ref                                        watchdog1_single_;
  field_ref                                        watchdog1_channel_;
  field_ref                                        watchdog1_filter_;
  std::array<field_ref, watchdogs>                 watchdog_flags_;
  std::array<field_ref, watchdogs>                 watchdog_low_;
  std::array<field_ref, watchdogs>                 watchdog_high_;
  std::array<field_ref, watchdogs>                 watchdog_channels_;
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

constexpr std::array<std::string_view, offsets> offset_registers{
    "ADC_OFR1", "ADC_OFR2", "ADC_OFR3", "ADC_OFR4"};

template <typename Traits>
adc_model<Traits>::adc_model(model_context& ctx, peripheral_desc const& desc, supply_options supply)
    : base{ctx, desc, Traits::unit_name, adc_writes<Traits>}, regs_{ctx.regs}, supply_{supply},
      inputs_{wiring<Traits>(desc.name)}, kernel_{ctx.clock.domain(lower(desc.name))},
      hclk_{ctx.clock.domain("hclk")}, sysclk_{ctx.clock.domain("sysclk")},
      line_{irq(desc.name + "_IRQn")}, request_{ctx.peers, lower(desc.name) + "_dma"},
      isr_{reg("ADC_ISR")}, enable_{field("ADC_CR", "ADEN")}, disable_{field("ADC_CR", "ADDIS")},
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
      bulb_{field("ADC_CFGR2", "BULB")}, sampling_trigger_{field("ADC_CFGR2", "SMPTRIG")},
      triggered_oversampling_{field("ADC_CFGR2", "TROVS")},
      differential_{field("ADC_DIFSEL", "DIFSEL")}, watchdog1_{field(Traits::config, "AWD1EN")},
      watchdog1_single_{field(Traits::config, "AWD1SGL")},
      watchdog1_channel_{field(Traits::config, "AWD1CH")},
      watchdog1_filter_{field(Traits::filter_register, Traits::filter_field)}
{
  bonded_channels_ = desc.bonded_channels.value_or(external_channels());
  if((bonded_channels_ & ~external_channels()) != 0)
  {
    throw config_error(desc.name, 0, "bonded_channels", "a mask of external channels");
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
  for(std::size_t index = 0; index < channels; ++index)
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
  auto const code  = which < channels ? get(regs_, sampling_[which]) : 0u;
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
    state_.sum += code();
    ++state_.held;
  }
}

template <typename Traits> auto adc_model<Traits>::ratio() const -> std::uint32_t
{
  return oversampled() ? Traits::ratio(get(regs_, oversampling_ratio_)) : 1u;
}

template <typename Traits> auto adc_model<Traits>::channel() const -> std::uint32_t
{
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
auto adc_model<Traits>::external(std::uint8_t channel) const -> std::size_t
{
  if(not Traits::external(channel))
  {
    throw config_error(
        desc_.name, 0, "channel",
        std::format("an external channel {}, not {}", Traits::externals, channel));
  }
  return channel;
}

template <typename Traits>
void adc_model<Traits>::drive(std::uint8_t channel, std::uint32_t microvolts)
{
  auto const at = external(channel);
  if(state_.converting)
  {
    hold_until(ctx_.clock.time().ps);
  }
  state_.input_uv[at] = microvolts;
}

template <typename Traits>
auto adc_model<Traits>::input(std::uint8_t channel) const -> std::uint32_t
{
  return state_.input_uv[external(channel)];
}

template <typename Traits> auto adc_model<Traits>::external_channels() const -> std::uint32_t
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
  return mask;
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
}

template <typename Traits> void adc_model<Traits>::on_bus_reset()
{
  auto const was = listened();
  restore_reset_values();
  retune(was);
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
  if(deep_power_down_.of(now) != 0)
  {
    now = regulator_.with(now, 0);
  }
  regs_.poke(enable_.offset, now);
  if constexpr(Traits::regulator_flag)
  {
    update(
        regs_, *regulator_ready_,
        regulator_.of(now) != 0 and deep_power_down_.of(now) == 0 ? 1u : 0u);
  }
  update(regs_, calibrate_, 0);
  if(injected_start_.of(now) != 0)
  {
    update(regs_, injected_start_, 0);
    warn_once(injected_logged_, "ADC_CR", "JADSTART: injected conversions");
  }
  if(disable_.of(now) != 0)
  {
    update(regs_, disable_, 0);
    update(regs_, enable_, 0);
    update(regs_, start_, 0);
    update(regs_, stop_, 0);
    abort();
    signal();
    refuse_start(refused);
    return;
  }
  if(enable_.of(now) != 0 and enable_.of(old) == 0)
  {
    if constexpr(Traits::ready_cycles.has_value())
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
  if(stop_.of(now) != 0)
  {
    update(regs_, stop_, 0);
    update(regs_, start_, 0);
    abort();
    signal();
    return;
  }
  if(start_.of(now) != 0 and start_.of(old) == 0)
  {
    state_.dma_stopped = false;
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
  if(end_of_sampling_.of(old) != 0 and get(regs_, end_of_sampling_) == 0 and state_.converting)
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
  if(get(regs_, discontinuous_) != 0 and get(regs_, continuous_) != 0)
  {
    forbidden(
        discontinuous_continuous_logged_, Traits::config,
        std::format(
            "DISCEN and CONT both set, forbidden by {}: converted as DISCEN alone",
            Traits::cite_discontinuous));
  }
  if(get(regs_, auto_delay_) != 0)
  {
    unmodelled(auto_delay_logged_, Traits::config, "AUTDLY: auto-delayed conversions");
  }
  if(get(regs_, bulb_) != 0)
  {
    unmodelled(bulb_logged_, "ADC_CFGR2", "BULB: bulb sampling mode");
  }
  if(get(regs_, sampling_trigger_) != 0)
  {
    unmodelled(
        sampling_trigger_logged_, "ADC_CFGR2", "SMPTRIG: sampling time control trigger mode");
  }
  if(get(regs_, triggered_oversampling_) != 0)
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
  if(Traits::filter_gates_dma
     and get(regs_, watchdog1_) != 0
     and get(regs_, watchdog1_filter_) != 0
     and dma() != dma_mode::off)
  {
    unmodelled(
        watchdog_filter_dma_logged_, "ADC_HTR1",
        "AWDFILT1 with DMNGT: DMA requests gated by the AWD1 window");
  }
  for(std::size_t rank = 0; rank < length(); ++rank)
  {
    check_unmodelled(channel_at(rank));
    check_preselection(channel_at(rank));
    check_option_switch(channel_at(rank));
    require_factory_word(channel_at(rank));
  }
}

template <typename Traits> void adc_model<Traits>::begin_sequence(std::uint64_t start_ps)
{
  prepare();
  if(get(regs_, discontinuous_) == 0 or state_.rank >= length())
  {
    state_.rank = 0;
  }
  state_.subgroup_left = static_cast<std::uint8_t>(get(regs_, discontinuous_count_) + 1u);
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
  if(channel < channels and (get(regs_, differential_) >> channel & 1u) != 0)
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
  for(auto const& offset : offsets_)
  {
    auto const active = offset.enable ? get(regs_, *offset.enable) != 0 and not alignment_ignored()
                                      : get(regs_, offset.value) != 0;
    if(active and get(regs_, offset.channel) == channel)
    {
      unmodelled(offset_logged_, offset.reg, "OFFSET: offset of the result");
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
  auto const clock     = source();
  state_.start_ps      = start_ps;
  state_.clock_hz      = clock.hz;
  state_.divider       = clock.divider;
  state_.held          = 0;
  state_.sum           = 0;
  state_.waiting_clock = false;
  state_.converting    = true;
  ++state_.generation;
  ctx_.events.schedule_at(
      vtime{after(std::uint64_t{ratio()} * conversion())}, conversion_end, state_.generation,
      ctx_.clock);
  if(get(regs_, end_of_sampling_) == 0)
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
  auto const end    = after(std::uint64_t{ratio()} * conversion());
  state_.converting = false;
  for(auto const samples = ratio(); state_.held < samples; ++state_.held)
  {
    state_.sum += code();
  }
  auto result = state_.sum;
  if(oversampled())
  {
    auto const shift = get(regs_, oversampling_shift_);
    result           = shift == 0 ? state_.sum : (state_.sum + (1u << (shift - 1u))) >> shift;
    result &= Traits::result_mask;
  }
  watch(channel(), result);
  land(result);
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
  if(get(regs_, discontinuous_) != 0)
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
    return get(regs_, watchdog1_) != 0
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
        Traits::compared(watchdog, result, get(regs_, resolution_), oversampled());
    auto out = compared < get(regs_, watchdog_low_[watchdog])
            or compared > get(regs_, watchdog_high_[watchdog]);
    if(watchdog == 0)
    {
      state_.filter_count =
          out ? std::min<std::uint8_t>(state_.filter_count + 1u, max_filter_count) : 0;
      out = state_.filter_count > get(regs_, watchdog1_filter_);
    }
    if(out)
    {
      update(regs_, watchdog_flags_[watchdog], 1);
    }
  }
}

template <typename Traits> void adc_model<Traits>::land(std::uint32_t result)
{
  if constexpr(not Traits::align.empty())
  {
    if(get(regs_, *align_) != 0 and not alignment_ignored())
    {
      result = Traits::left_aligned(result, get(regs_, resolution_));
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

template <typename Traits> auto adc_model<Traits>::listens(std::string_view signal) const -> bool
{
  auto const selected = listened();
  auto const named    = reflex::to_enum_value<typename Traits::trigger_enum>(signal);
  return selected and named and std::to_underlying(*named) == *selected;
}

template <typename Traits>
void adc_model<Traits>::triggered(std::string_view signal, trigger_edge edge, std::uint64_t at_ps)
{
  auto const polarity = get(regs_, trigger_);
  auto const detected = edge == trigger_edge::rising ? (polarity & exten_rising) != 0
                                                     : (polarity & exten_falling) != 0;
  if(not detected or busy() or not listens(signal))
  {
    return;
  }
  begin_sequence(at_ps + trigger_latency_ps(at_ps));
}

template <typename Traits>
auto adc_model<Traits>::trigger_latency_ps(std::uint64_t at_ps) -> std::uint64_t
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
  return edge
       + cycles_ps(
             Traits::trigger_latency_quarters(clock.divider, mode), 4, clock.divider, clock.hz);
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
        ctx_.peers, trigger_name<typename Traits::trigger_enum>(trigger_select_.of(config))));
  }
  if(trigger_.of(next) != 0 and (not external or moved))
  {
    static_cast<void>(interest_changed(
        ctx_.peers, trigger_name<typename Traits::trigger_enum>(trigger_select_.of(next))));
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
        interest_changed(ctx_.peers, trigger_name<typename Traits::trigger_enum>(*was)));
  }
  if(not now)
  {
    return;
  }
  auto const signal = trigger_name<typename Traits::trigger_enum>(*now);
  if(not interest_changed(ctx_.peers, signal))
  {
    unmodelled(
        unproduced_logged_[*now], Traits::config,
        signal.starts_with("adc_ext_trg")
            ? std::format("EXTSEL {}: reserved trigger selection", *now)
            : std::format("EXTSEL {}: trigger {}", *now, signal));
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
  abort();
  if constexpr(Traits::dma_end_stops)
  {
    update(regs_, start_, 0);
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
  if(Traits::external(channel) and not option_open(channel))
  {
    return code_of<Traits>(state_.input_uv[channel], supply_.vdda_mv);
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
  if(not std::exchange(logged, true))
  {
    warn(name, std::move(message));
  }
  if(ctx_.strict_peripherals)
  {
    throw model_error(desc_.name, std::string{name}, reg(name).offset);
  }
}

template <typename Traits>
void adc_model<Traits>::unmodelled(bool& logged, std::string_view name, std::string_view what)
{
  warn_once(logged, name, what);
  if(ctx_.strict_peripherals)
  {
    throw model_error(desc_.name, std::string{name}, reg(name).offset);
  }
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
}
