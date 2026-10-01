#include "factories.hpp"
#include "model_base.hpp"

#include <array>
#include <cstdint>
#include <format>
#include <memory>
#include <optional>
#include <utility>

namespace popcorn::periph::stm32
{
namespace
{
constexpr address       ts_cal1_address     = 0x0BFA'0710;
constexpr address       vrefint_cal_address = 0x0BFA'07A5;
constexpr std::uint32_t vrefint_channel     = 0;
constexpr std::uint32_t sensor_channel      = 19;
constexpr std::uint32_t external_code       = 0;

struct adc_state
{
  friend auto operator==(adc_state const&, adc_state const&) -> bool = default;
};

[[nodiscard]] auto factory_halfword(mcu_description const& part, address at)
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

/**
 * @brief ADC12 common registers of the STM32U5, RM0456 section 33.7: ADC12_CCR connects
 *        VREFINT with VREFEN and the temperature sensor with VSENSESEL to the ADC1 internal
 *        channels. Storage otherwise.
 */
class adc_common final : public dispatch_model<adc_common, adc_state>
{
public:
  adc_common(model_context& ctx, peripheral_desc const& desc);

  void reset(register_file& regs, reset_kind kind) override;
  void on_event(std::uint32_t, std::uint64_t) override
  {}

  void ccr_written(std::uint32_t, std::uint32_t)
  {}

  [[nodiscard]] auto vrefint_connected() const -> bool
  {
    return get(regs_, vrefint_) != 0;
  }

  [[nodiscard]] auto sensor_connected() const -> bool
  {
    return get(regs_, sensor_) != 0;
  }

private:
  register_file& regs_;
  field_ref      vrefint_;
  field_ref      sensor_;
};

constexpr std::array<write_entry<adc_common>, 1> adc_common_writes{
    {
     {"ADC12_CCR", &adc_common::ccr_written},
     }
};

adc_common::adc_common(model_context& ctx, peripheral_desc const& desc)
    : dispatch_model{ctx, desc, "ADC12", adc_common_writes}, regs_{ctx.regs},
      vrefint_{field("ADC12_CCR", "VREFEN")}, sensor_{field("ADC12_CCR", "VSENSESEL")}
{}

void adc_common::reset(register_file& regs, reset_kind)
{
  for(auto const& name : claims())
  {
    regs.poke(reg(name).offset, describe(name).reset);
  }
}

/**
 * @brief ADC1 of the STM32U5, RM0456 section 33, its power-up handshake and software-triggered
 *        regular conversions, done at the write with no virtual time passing. ADVREGEN out of
 *        deep power-down sets LDORDY, DEEPPWD clears ADVREGEN, ADCAL clears at once, ADEN sets
 *        ADRDY, ADDIS clears ADEN. ADSTART converts the SQ1 channel once: ADC_DR takes the code,
 *        EOC and EOS set, ADSTART clears, and a CPU or DMA read of ADC_DR clears EOC. VDDA =
 *        VREF+ = 3.0 V at 30 C: VREFINT reads the factory VREFINT_CAL and the temperature sensor
 *        TS_CAL1 when ADC12_CCR connects them, and 0 otherwise. Each word is read from the
 *        description's signature content at its first conversion, a config_error when absent.
 *        Every other channel reads 0. A 14-bit code loses two bits per RES step. Continuous,
 *        discontinuous, triggered and multi-channel sequences, injected conversions, DMA,
 *        interrupts, offsets, gain and watchdogs are not modelled and log a warning once.
 */
class adc final : public dispatch_model<adc, adc_state>
{
public:
  adc(model_context& ctx, peripheral_desc const& desc);

  void reset(register_file& regs, reset_kind kind) override;
  void on_event(std::uint32_t, std::uint64_t) override
  {}

  auto on_read(register_file& regs, std::uint32_t offset, std::uint8_t size, access_ctx ctx)
      -> std::expected<std::uint32_t, bus_error> override;

  void cr_written(std::uint32_t old, std::uint32_t now);
  void dr_written(std::uint32_t, std::uint32_t)
  {}

private:
  [[nodiscard]] auto calibration(address at, std::string_view name) const -> std::uint16_t;
  [[nodiscard]] auto sample(std::uint32_t channel) const -> std::uint32_t;
  void               convert();
  void               warn_once(bool& logged, std::string_view reg, std::string_view what);

  register_file& regs_;
  field_ref      enable_;
  field_ref      disable_;
  field_ref      start_;
  field_ref      stop_;
  field_ref      injected_start_;
  field_ref      regulator_;
  field_ref      deep_power_down_;
  field_ref      calibrate_;
  field_ref      ready_;
  field_ref      end_of_conversion_;
  field_ref      end_of_sequence_;
  field_ref      regulator_ready_;
  field_ref      resolution_;
  field_ref      continuous_;
  field_ref      discontinuous_;
  field_ref      trigger_;
  field_ref      length_;
  field_ref      first_channel_;
  field_ref      data_;
  std::uint32_t  set_only_{};
  bool           sequence_logged_{};
  bool           injected_logged_{};
};

constexpr std::array<write_entry<adc>, 2> adc_writes{
    {
     {"ADC_CR", &adc::cr_written},
     {"ADC_DR", &adc::dr_written},
     }
};

adc::adc(model_context& ctx, peripheral_desc const& desc)
    : dispatch_model{ctx, desc, "ADC", adc_writes}, regs_{ctx.regs},
      enable_{field("ADC_CR", "ADEN")}, disable_{field("ADC_CR", "ADDIS")},
      start_{field("ADC_CR", "ADSTART")}, stop_{field("ADC_CR", "ADSTP")},
      injected_start_{field("ADC_CR", "JADSTART")}, regulator_{field("ADC_CR", "ADVREGEN")},
      deep_power_down_{field("ADC_CR", "DEEPPWD")}, calibrate_{field("ADC_CR", "ADCAL")},
      ready_{field("ADC_ISR", "ADRDY")}, end_of_conversion_{field("ADC_ISR", "EOC")},
      end_of_sequence_{field("ADC_ISR", "EOS")}, regulator_ready_{field("ADC_ISR", "LDORDY")},
      resolution_{field("ADC_CFGR1", "RES")}, continuous_{field("ADC_CFGR1", "CONT")},
      discontinuous_{field("ADC_CFGR1", "DISCEN")}, trigger_{field("ADC_CFGR1", "EXTEN")},
      length_{field("ADC_SQR1", "L")}, first_channel_{field("ADC_SQR1", "SQ1")},
      data_{field("ADC_DR", "RDATA")}
{
  for(auto const bit : {enable_, disable_, start_, stop_, injected_start_, calibrate_})
  {
    set_only_ |= bit.mask();
  }
  set_only_ |= field("ADC_CR", "JADSTP").mask();
}

auto adc::calibration(address at, std::string_view name) const -> std::uint16_t
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

void adc::reset(register_file& regs, reset_kind)
{
  for(auto const& name : claims())
  {
    regs.poke(reg(name).offset, describe(name).reset);
  }
}

auto adc::on_read(register_file& regs, std::uint32_t offset, std::uint8_t size, access_ctx ctx)
    -> std::expected<std::uint32_t, bus_error>
{
  auto const value = dispatch_model::on_read(regs, offset, size, ctx);
  if((offset & ~3u) == data_.offset and ctx.by != initiator::debugger)
  {
    update(regs_, end_of_conversion_, 0);
  }
  return value;
}

void adc::cr_written(std::uint32_t old, std::uint32_t now)
{
  now |= old & set_only_;
  if(deep_power_down_.of(now) != 0)
  {
    now = regulator_.with(now, 0);
  }
  regs_.poke(enable_.offset, now);
  update(
      regs_, regulator_ready_, regulator_.of(now) != 0 and deep_power_down_.of(now) == 0 ? 1u : 0u);
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
    return;
  }
  if(enable_.of(now) != 0 and enable_.of(old) == 0 and get(regs_, regulator_ready_) != 0)
  {
    update(regs_, ready_, 1);
  }
  if(stop_.of(now) != 0)
  {
    update(regs_, stop_, 0);
    update(regs_, start_, 0);
    return;
  }
  if(start_.of(now) != 0 and enable_.of(now) != 0)
  {
    convert();
  }
}

auto adc::sample(std::uint32_t channel) const -> std::uint32_t
{
  auto const* const common = dynamic_cast<adc_common const*>(ctx_.peers.find("ADC12"));
  if(channel == vrefint_channel)
  {
    return common != nullptr and common->vrefint_connected()
             ? calibration(vrefint_cal_address, "VREFINT_CAL")
             : 0u;
  }
  if(channel == sensor_channel)
  {
    return common != nullptr and common->sensor_connected()
             ? calibration(ts_cal1_address, "TS_CAL1")
             : 0u;
  }
  return external_code;
}

void adc::convert()
{
  auto const unmodelled = get(regs_, continuous_)
                        + get(regs_, discontinuous_)
                        + get(regs_, trigger_)
                        + get(regs_, length_);
  if(unmodelled != 0)
  {
    warn_once(
        sequence_logged_, "ADC_CFGR1",
        "CONT, DISCEN, EXTEN or SQR1.L: continuous, discontinuous, triggered or multi-channel "
        "sequences, SQ1 converted once");
  }
  auto const code = sample(get(regs_, first_channel_)) >> (2 * get(regs_, resolution_));
  update(regs_, data_, code);
  update(regs_, end_of_conversion_, 1);
  update(regs_, end_of_sequence_, 1);
  update(regs_, start_, 0);
}

void adc::warn_once(bool& logged, std::string_view reg, std::string_view what)
{
  if(std::exchange(logged, true))
  {
    return;
  }
  warn(reg, std::format("{} not modelled", what));
}
}

auto adc_factory() -> model_factory
{
  return [](model_context& ctx, peripheral_desc const& desc) -> std::shared_ptr<model> {
    return std::make_shared<adc>(ctx, desc);
  };
}

auto adc_common_factory() -> model_factory
{
  return [](model_context& ctx, peripheral_desc const& desc) -> std::shared_ptr<model> {
    return std::make_shared<adc_common>(ctx, desc);
  };
}
}
