#include "factories.hpp"
#include "model_base.hpp"

#include <reflex/enum.hpp>

#include <array>
#include <cstdint>
#include <format>
#include <memory>
#include <utility>

namespace popcorn::periph::stm32
{
namespace
{
constexpr std::uint32_t voltage_ready = 0;
constexpr std::uint32_t booster_ready = 1;

enum class low_power_mode : std::uint8_t
{
  stop0,
  stop1,
  stop2,
  stop3,
  standby,
  shutdown,
};

constexpr std::array<low_power_mode, 8> modes_by_lpms{
    low_power_mode::stop0,    low_power_mode::stop1,   low_power_mode::stop2,
    low_power_mode::stop3,    low_power_mode::standby, low_power_mode::standby,
    low_power_mode::shutdown, low_power_mode::shutdown};

struct pwr_state
{
  std::array<std::uint32_t, 2> generation{};

  friend auto operator==(pwr_state const&, pwr_state const&) -> bool = default;
};

/**
 * @brief PWR voltage scaling of the STM32U5, RM0456 section 10.10.4: VOSRDY follows a VOS change
 *        and BOOSTRDY follows BOOSTEN after the board's ready delay, and PWR_SVMSR.ACTVOS takes
 *        the new range with VOSRDY. Low-power modes are not modelled: the first write to
 *        PWR_CR1 logs one warning per machine life naming the LPMS mode, and a deep-sleep WFI idles
 * as a plain WFI.
 */
class pwr final : public dispatch_model<pwr, pwr_state>
{
public:
  pwr(model_context& ctx, peripheral_desc const& desc, pwr_options const& options);

  void reset(register_file& regs, reset_kind kind) override;
  void on_event(std::uint32_t kind, std::uint64_t payload) override;

  void vosr_written(std::uint32_t old, std::uint32_t now);
  void cr1_written(std::uint32_t old, std::uint32_t now);

private:
  pwr_options    options_;
  register_file& regs_;
  field_ref      range_;
  field_ref      range_ready_;
  field_ref      booster_;
  field_ref      booster_ready_;
  field_ref      active_range_;
  field_ref      active_range_ready_;
  field_ref      low_power_;
  bool           low_power_logged_{};
};

constexpr std::array<write_entry<pwr>, 2> pwr_writes{
    {
     {"PWR_CR1", &pwr::cr1_written},
     {"PWR_VOSR", &pwr::vosr_written},
     }
};

pwr::pwr(model_context& ctx, peripheral_desc const& desc, pwr_options const& options)
    : dispatch_model{ctx, desc, "PWR", pwr_writes}, options_{options}, regs_{ctx.regs},
      range_{field("PWR_VOSR", "VOS")}, range_ready_{field("PWR_VOSR", "VOSRDY")},
      booster_{field("PWR_VOSR", "BOOSTEN")}, booster_ready_{field("PWR_VOSR", "BOOSTRDY")},
      active_range_{field("PWR_SVMSR", "ACTVOS")},
      active_range_ready_{field("PWR_SVMSR", "ACTVOSRDY")}, low_power_{field("PWR_CR1", "LPMS")}
{}

void pwr::reset(register_file& regs, reset_kind)
{
  for(auto const& name : claims())
  {
    regs.poke(reg(name).offset, describe(name).reset);
  }
}

void pwr::vosr_written(std::uint32_t old, std::uint32_t now)
{
  if(range_.of(now) != range_.of(old))
  {
    update(regs_, range_ready_, 0);
    update(regs_, active_range_ready_, 0);
    schedule_in(options_.ready_delay, voltage_ready, ++state_.generation[voltage_ready]);
  }
  if(booster_.of(now) != booster_.of(old))
  {
    update(regs_, booster_ready_, 0);
    auto const generation = ++state_.generation[booster_ready];
    if(booster_.of(now) != 0)
    {
      schedule_in(options_.ready_delay, booster_ready, generation);
    }
  }
}

void pwr::cr1_written(std::uint32_t, std::uint32_t now)
{
  if(std::exchange(low_power_logged_, true))
  {
    return;
  }
  warn(
      "PWR_CR1", std::format(
                     "LPMS selects {}, not modelled: a deep-sleep WFI idles as a plain WFI",
                     reflex::enum_value_name(modes_by_lpms[low_power_.of(now)])));
}

void pwr::on_event(std::uint32_t kind, std::uint64_t payload)
{
  if(kind > booster_ready or payload != state_.generation[kind])
  {
    return;
  }
  if(kind == voltage_ready)
  {
    update(regs_, range_ready_, 1);
    update(regs_, active_range_, get(regs_, range_));
    update(regs_, active_range_ready_, 1);
  }
  else if(get(regs_, booster_) != 0)
  {
    update(regs_, booster_ready_, 1);
  }
}
}

auto pwr_factory(pwr_options options) -> model_factory
{
  return [options](model_context& ctx, peripheral_desc const& desc) -> std::shared_ptr<model> {
    return std::make_shared<pwr>(ctx, desc, options);
  };
}
}
