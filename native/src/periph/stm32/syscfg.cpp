#include "factories.hpp"
#include "gtzc.hpp"
#include "model_base.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <optional>
#include <string_view>

namespace popcorn::periph::stm32
{
namespace
{
struct syscfg_state
{
  friend auto operator==(syscfg_state const&, syscfg_state const&) -> bool = default;
};

/**
 * @brief SYSCFG of the STM32U5, RM0456 section 15, storage with the TrustZone rules of RM0456
 *        15.2.2 and Table 131. `SYSCFG_SECCFGR` reads for every transaction and takes Secure
 *        writes, FPUSEC privileged ones only. A Non-secure write to it is ignored with an
 *        illegal access event. `SYSCFG_CSLCKR` is Secure and privileged, `SYSCFG_FPUIMR` Secure
 *        with FPUSEC set and privileged, `SYSCFG_CNSLCKR` privileged, `SYSCFG_CFGR2` Secure with
 *        CLASSBSEC set, `SYSCFG_MESR`, `SYSCFG_CCCSR`, `SYSCFG_CCVR` and `SYSCFG_CCCR` Secure
 *        with SYSCFGSEC set. A Non-secure access to a Secure one of these is RAZ/WI with the
 *        event, an unprivileged access to a privileged one RAZ/WI. In `SYSCFG_CFGR1`, PBn_FMP
 *        is Secure while pin PBn is Secure in GPIOB, BOOSTEN and ANASWVDD while the TZSC makes
 *        ADC1 or ADC4 Secure, RAZ/WI to a Non-secure access with no event. SYSCFGSEC makes the
 *        RCC SYSCFG clock bits Secure. The CSLCKR and CNSLCKR locks are stored, they do not
 *        lock the core's SAU, MPU, VTOR or AIRCR. `SYSCFG_RSSCMDR` and the OTG_HS PHY registers
 *        have no rule. Under TZEN=0 `SYSCFG_SECCFGR` is RAZ/WI, its storage keeps its reset,
 *        and every other register is storage. The model is stateless: a save made by a machine
 *        without it restores it from the register storage.
 */
class syscfg final : public dispatch_model<syscfg, syscfg_state>, public trustzone_aware
{
public:
  syscfg(model_context& ctx, peripheral_desc const& desc);

  void reset(register_file& regs, reset_kind kind) override;
  void on_event(std::uint32_t kind, std::uint64_t payload) override;

  [[nodiscard]] auto secure_function() const -> bool override;
  [[nodiscard]] auto secure_word() const -> std::uint32_t override;
  [[nodiscard]] auto stateless() const -> bool override;
  [[nodiscard]] auto guard(std::uint32_t word, access_ctx const& ctx, bool write) -> bus_guard;

private:
  [[nodiscard]] auto register_guard(std::uint32_t word, access_ctx const& ctx) -> bus_guard;
  [[nodiscard]] auto secure_peripheral(std::optional<std::size_t> unit) const -> bool;

  register_file&               regs_;
  reg_ref                      secure_;
  field_ref                    system_;
  field_ref                    class_b_;
  field_ref                    fpu_;
  std::uint32_t                fpu_mask_{};
  std::uint32_t                config1_{};
  std::uint32_t                config2_{};
  std::uint32_t                secure_lock_{};
  std::uint32_t                nonsecure_lock_{};
  std::array<std::uint32_t, 4> compensation_{};
  std::array<field_ref, 4>     fast_mode_{};
  std::uint32_t                analog_{};
  std::optional<std::size_t>   adc1_;
  std::optional<std::size_t>   adc4_;
  std::optional<std::size_t>   unit_;
  illegal_event                illegal_;
};

constexpr std::array<write_entry<syscfg>, 1> syscfg_writes{
    {
     {"SYSCFG_SECCFGR", nullptr},
     }
};

constexpr auto syscfg_trustzone_writes = std::to_array<write_entry<syscfg>>({
    {"SYSCFG_CFGR1",   nullptr},
    {"SYSCFG_FPUIMR",  nullptr},
    {"SYSCFG_CNSLCKR", nullptr},
    {"SYSCFG_CSLCKR",  nullptr},
    {"SYSCFG_CFGR2",   nullptr},
    {"SYSCFG_MESR",    nullptr},
    {"SYSCFG_CCCSR",   nullptr},
    {"SYSCFG_CCVR",    nullptr},
    {"SYSCFG_CCCR",    nullptr},
});

syscfg::syscfg(model_context& ctx, peripheral_desc const& desc)
    : dispatch_model{ctx, desc, "SYSCFG", syscfg_writes, syscfg_trustzone_writes}, regs_{ctx.regs},
      secure_{reg("SYSCFG_SECCFGR")}, system_{field("SYSCFG_SECCFGR", "SYSCFGSEC")},
      class_b_{field("SYSCFG_SECCFGR", "CLASSBSEC")}, fpu_{field("SYSCFG_SECCFGR", "FPUSEC")},
      adc1_{unit_named(ctx.part, "ADC1")}, adc4_{unit_named(ctx.part, "ADC4")},
      unit_{unit_named(ctx.part, desc.name)}
{
  if(not ctx.trustzone)
  {
    return;
  }
  fpu_mask_       = reg("SYSCFG_FPUIMR").offset;
  config1_        = reg("SYSCFG_CFGR1").offset;
  config2_        = reg("SYSCFG_CFGR2").offset;
  secure_lock_    = reg("SYSCFG_CSLCKR").offset;
  nonsecure_lock_ = reg("SYSCFG_CNSLCKR").offset;
  compensation_   = {
      reg("SYSCFG_MESR").offset, reg("SYSCFG_CCCSR").offset, reg("SYSCFG_CCVR").offset,
      reg("SYSCFG_CCCR").offset};
  for(std::size_t at = 0; at < fast_mode_.size(); ++at)
  {
    fast_mode_[at] = field("SYSCFG_CFGR1", std::format("PB{}_FMP", at + 6));
  }
  analog_ = field("SYSCFG_CFGR1", "BOOSTEN").mask() | field("SYSCFG_CFGR1", "ANASWVDD").mask();
}

void syscfg::reset(register_file& regs, reset_kind)
{
  for(auto const& name : claims())
  {
    regs.poke(reg(name).offset, describe(name).reset);
  }
}

void syscfg::on_event(std::uint32_t, std::uint64_t)
{}

auto syscfg::secure_word() const -> std::uint32_t
{
  return ctx_.trustzone ? regs_.peek(secure_.offset) : 0;
}

auto syscfg::stateless() const -> bool
{
  return true;
}

auto syscfg::secure_function() const -> bool
{
  return system_.of(secure_word()) != 0;
}

auto syscfg::secure_peripheral(std::optional<std::size_t> unit) const -> bool
{
  return unit and unit_secure(ctx_.peers, ctx_.part, *unit);
}

auto syscfg::guard(std::uint32_t word, access_ctx const& ctx, bool write) -> bus_guard
{
  auto const all = ~std::uint32_t{};
  if(word == secure_.offset)
  {
    if(not ctx_.trustzone)
    {
      return bus_guard::both(all);
    }
    if(not ctx.secure)
    {
      if(write and unit_)
      {
        illegal_.raise(ctx_.peers, *unit_, ctx.by);
      }
      return {.wi = all};
    }
    return {.wi = ctx.privileged ? 0 : fpu_.mask()};
  }
  if(not ctx_.trustzone)
  {
    return {};
  }
  return register_guard(word, ctx);
}

auto syscfg::register_guard(std::uint32_t word, access_ctx const& ctx) -> bus_guard
{
  auto const all      = ~std::uint32_t{};
  auto const security = regs_.peek(secure_.offset);
  auto const refused  = [&](bool secure, bool privileged) {
    if(secure and not ctx.secure)
    {
      if(unit_)
      {
        illegal_.raise(ctx_.peers, *unit_, ctx.by);
      }
      return bus_guard::both(all);
    }
    return guard_of(all, false, privileged, ctx);
  };
  if(word == secure_lock_)
  {
    return refused(true, true);
  }
  if(word == fpu_mask_)
  {
    return refused(fpu_.of(security) != 0, true);
  }
  if(word == nonsecure_lock_)
  {
    return refused(false, true);
  }
  if(word == config2_)
  {
    return refused(class_b_.of(security) != 0, false);
  }
  if(std::ranges::contains(compensation_, word))
  {
    return refused(system_.of(security) != 0, false);
  }
  if(word != config1_ or ctx.secure)
  {
    return {};
  }
  std::uint32_t hidden = 0;
  auto const*   port   = dynamic_cast<trustzone_aware const*>(ctx_.peers.find("GPIOB"));
  auto const    pins   = port != nullptr ? port->secure_word() : 0;
  for(std::size_t at = 0; at < fast_mode_.size(); ++at)
  {
    if((pins >> (at + 6) & 1u) != 0)
    {
      hidden |= fast_mode_[at].mask();
    }
  }
  if(secure_peripheral(adc1_) or secure_peripheral(adc4_))
  {
    hidden |= analog_;
  }
  return bus_guard::both(hidden);
}
}

auto syscfg_factory() -> model_factory
{
  return [](model_context& ctx, peripheral_desc const& desc) -> std::shared_ptr<model> {
    return std::make_shared<syscfg>(ctx, desc);
  };
}
}
