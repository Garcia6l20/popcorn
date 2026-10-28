#include "factories.hpp"
#include "model_base.hpp"
#include "secure_config.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace popcorn::periph::stm32
{
namespace
{
struct syscfg_state
{
  friend auto operator==(syscfg_state const&, syscfg_state const&) -> bool = default;
};

static_assert(std::is_empty_v<syscfg_state>);

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
 *        and every other register is storage. Every register but `SYSCFG_SECCFGR` is storage
 *        the model does not model, at TZEN=0 and 1: a firmware write that lands, past the Table
 *        131 rules, logs one model_warning per register, and raises model_error under
 *        strict_peripherals before it is stored. Firmware reads are silent. A host view access
 *        to such a register raises model_error, peek and poke reach it. Deviation: RM0456 Table
 *        131 makes `SYSCFG_CSLCKR` RAZ/WI at TZEN=0, the model keeps it storage, P1 covers
 *        `SYSCFG_SECCFGR` only. The model is stateless: a save made by a machine without it
 *        restores it from the register storage.
 */
class syscfg final : public dispatch_model<syscfg, syscfg_state>,
                     public trustzone_aware,
                     public bus_reset_listener
{
public:
  syscfg(model_context& ctx, peripheral_desc const& desc);

  void reset(register_file& regs, reset_kind kind) override;
  void on_bus_reset() override;
  void on_held_write(std::uint32_t offset, access_ctx ctx) override;
  void on_event(std::uint32_t kind, std::uint64_t payload) override;

  [[nodiscard]] auto secure_function() const -> bool override;
  [[nodiscard]] auto secure_word() const -> std::uint32_t override;
  [[nodiscard]] auto stateless() const -> bool override;
  [[nodiscard]] auto storage_only(std::uint32_t offset) const -> bool override;
  [[nodiscard]] auto guard(std::uint32_t word, access_ctx const& ctx, bool write) -> bus_guard;

private:
  void               restore_reset_values();
  [[nodiscard]] auto rule(std::uint32_t word, access_ctx const& ctx, bool write) -> bus_guard;

  register_file&      regs_;
  reg_ref             secure_;
  secure_config_rules rules_;
  storage_notes       notes_;
};

constexpr std::array<std::string_view, 3> system_reset_only{
    "SYSCFG_CFGR2", "SYSCFG_CSLCKR", "SYSCFG_CNSLCKR"};

auto syscfg_writes(peripheral_desc const& desc) -> std::vector<write_entry<syscfg>>
{
  std::vector<write_entry<syscfg>> out;
  for(auto const& described : desc.registers)
  {
    if(not described.alternate_of)
    {
      out.push_back({described.name, nullptr});
    }
  }
  return out;
}

syscfg::syscfg(model_context& ctx, peripheral_desc const& desc)
    : dispatch_model{ctx, desc, "SYSCFG", syscfg_writes(desc)}, regs_{ctx.regs},
      secure_{reg("SYSCFG_SECCFGR")}, rules_{
                                          .system  = field("SYSCFG_SECCFGR", "SYSCFGSEC"),
                                          .class_b = field("SYSCFG_SECCFGR", "CLASSBSEC"),
                                          .fpu     = field("SYSCFG_SECCFGR", "FPUSEC"),
                                          .unit    = unit_named(ctx.part, desc.name)}
{
  if(not ctx.trustzone)
  {
    return;
  }
  rules_.fpu_mask       = reg("SYSCFG_FPUIMR").offset;
  rules_.pin_config     = reg("SYSCFG_CFGR1").offset;
  rules_.config2        = reg("SYSCFG_CFGR2").offset;
  rules_.secure_lock    = reg("SYSCFG_CSLCKR").offset;
  rules_.nonsecure_lock = reg("SYSCFG_CNSLCKR").offset;
  rules_.compensation   = {
      reg("SYSCFG_MESR").offset, reg("SYSCFG_CCCSR").offset, reg("SYSCFG_CCVR").offset,
      reg("SYSCFG_CCCR").offset};
  for(std::size_t at = 0; at < rules_.fast_mode.size(); ++at)
  {
    rules_.fast_mode[at] = field("SYSCFG_CFGR1", std::format("PB{}_FMP", at + 6));
  }
  auto const analog =
      field("SYSCFG_CFGR1", "BOOSTEN").mask() | field("SYSCFG_CFGR1", "ANASWVDD").mask();
  rules_.owned = {
      owned_bits{analog, unit_named(ctx.part, "ADC1")},
      owned_bits{analog, unit_named(ctx.part, "ADC4")}
  };
}

void syscfg::reset(register_file&, reset_kind)
{
  restore_reset_values();
}

void syscfg::on_held_write(std::uint32_t offset, access_ctx ctx)
{
  static_cast<void>(guard(offset & ~3u, ctx, true));
}

void syscfg::on_bus_reset()
{
  std::vector<std::pair<std::uint32_t, std::uint32_t>> kept;
  for(auto const name : system_reset_only)
  {
    if(has_register(name))
    {
      auto const at = reg(name).offset;
      kept.emplace_back(at, regs_.peek(at));
    }
  }
  restore_reset_values();
  for(auto const& [at, value] : kept)
  {
    regs_.poke(at, value);
  }
}

void syscfg::restore_reset_values()
{
  for(auto const& name : claims())
  {
    regs_.poke(reg(name).offset, describe(name).reset);
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
  return rules_.system.of(secure_word()) != 0;
}

auto syscfg::storage_only(std::uint32_t offset) const -> bool
{
  return offset != secure_.offset;
}

auto syscfg::guard(std::uint32_t word, access_ctx const& ctx, bool write) -> bus_guard
{
  auto const result = rule(word, ctx, write);
  if(write and storage_only(word))
  {
    notes_.written(ctx_, desc_, "SYSCFG", word, ctx, result);
  }
  return result;
}

auto syscfg::rule(std::uint32_t word, access_ctx const& ctx, bool write) -> bus_guard
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
      if(write and rules_.unit)
      {
        rules_.illegal.raise(ctx_.peers, *rules_.unit, ctx.by);
      }
      return {.wi = all};
    }
    return {.wi = ctx.privileged ? 0 : rules_.fpu.mask()};
  }
  if(not ctx_.trustzone)
  {
    return {};
  }
  return rules_.guard(word, regs_.peek(secure_.offset), ctx, ctx_.peers, ctx_.part)
      .value_or(bus_guard{});
}
}

auto syscfg_factory() -> model_factory
{
  return [](model_context& ctx, peripheral_desc const& desc) -> std::shared_ptr<model> {
    return std::make_shared<syscfg>(ctx, desc);
  };
}
}
