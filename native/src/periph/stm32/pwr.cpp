#include "factories.hpp"
#include "family_names.hpp"
#include "gtzc.hpp"
#include "model_base.hpp"
#include "voltage_range.hpp"

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

constexpr std::array<low_power_mode, 2> h5_modes_by_lpms{
    low_power_mode::stop0, low_power_mode::standby};

constexpr std::array<low_power_mode, 8> modes_by_lpms{
    low_power_mode::stop0,    low_power_mode::stop1,   low_power_mode::stop2,
    low_power_mode::stop3,    low_power_mode::standby, low_power_mode::standby,
    low_power_mode::shutdown, low_power_mode::shutdown};

enum class pwr_role : std::uint8_t
{
  vos,
  vos_ready,
  active_vos,
  active_vos_ready,
  mode,
  booster,
  booster_ready,
};

enum class guard_source : std::uint8_t
{
  own,
  system_clock,
  port_pin,
  peripheral,
};

struct pwr_rule
{
  std::uint32_t              offset{};
  std::uint32_t              bits{};
  guard_source               source{};
  field_ref                  own{};
  std::string                peer{};
  std::uint8_t               pin{};
  std::optional<std::size_t> unit{};
};

struct pwr_state
{
  std::array<std::uint32_t, 2> generation{};

  friend auto operator==(pwr_state const&, pwr_state const&) -> bool = default;
};

class pwr;

/** @brief What differs between the PWR blocks one model serves. */
struct pwr_family
{
  /** @brief Register and field of every role, an empty name for a role the family lacks. */
  std::span<named_field<pwr_role> const> fields;
  /** @brief Low-power mode of every value of the mode field. */
  std::span<low_power_mode const> modes;
  /** @brief Performance level, 0 the highest, of every value of the VOS field. */
  std::span<std::uint8_t const> vos_levels;
  /** @brief The VOS ready bit reads 1 when the voltage is ready. */
  bool vos_ready_set = true;
  /** @brief The active VOS ready bit reads 1 when the voltage is ready. */
  bool active_ready_set = true;
  /** @brief Write handlers of every machine. */
  std::span<write_entry<pwr> const> writes;
  /** @brief Write handlers added under TrustZone. */
  std::span<write_entry<pwr> const> trustzone_writes;
};

/**
 * @brief PWR voltage scaling of the STM32U5, RM0456 section 10.10.4, and of the STM32H5, RM0481
 *        10.11.5 and 10.11.6. A VOS write that steps to a higher voltage clears VOSRDY until the
 *        board's ready delay ends, one that steps to a lower voltage keeps VOSRDY, the voltage
 *        being at or above the selected level, RM0456 10.10.4 and RM0481 10.11.6. ACTVOSRDY clears
 *        on every VOS change until the delay ends, and BOOSTRDY follows BOOSTEN the same way on the
 *        U5. ACTVOS takes the new range at the end of the delay and the FLASH model is told for its
 *        latency check. The H5 reads them in PWR_VOSSR and writes VOS in PWR_VOSCR.
 *        Low-power modes are not modelled: the first write to the mode register, PWR_CR1 on the
 *        U5 and PWR_PMCR on the H5, logs one warning per machine life naming the LPMS mode, and
 *        a deep-sleep WFI idles as a plain WFI.
 *
 *        TrustZone, RM0456 10.8 and Table 109: the bits a `PWR_SECCFGR` bit secures, the VOSR
 *        bits `RCC_SECCFGR.SYSCLKSEC` secures, the PUCRx and PDCRx bits of the Secure pins of
 *        GPIOx and `PWR_UCPDR` while the TZSC makes UCPD1 Secure are RAZ/WI to a Non-secure
 *        access, with no event. `PWR_PRIVCFGR.SPRIV` makes the Secure bits privileged, `NSPRIV`
 *        the securable bits left Non-secure, RAZ/WI to an unprivileged access. The status bits
 *        Table 109 does not list stay readable by every transaction. `PWR_SECCFGR` reads for
 *        every transaction and takes Secure writes, privileged with SPRIV set. A Non-secure
 *        write to it is ignored with an illegal access event. `PWR_PRIVCFGR` takes privileged
 *        writes, SPRIV Secure ones only. Under TZEN=0 `PWR_SECCFGR` is RAZ/WI, its storage
 *        keeps its reset, and `PWR_PRIVCFGR` is storage.
 */
class pwr final : public dispatch_model<pwr, pwr_state>,
                  public voltage_range_source,
                  public trustzone_aware
{
public:
  pwr(model_context&         ctx,
      peripheral_desc const& desc,
      pwr_family const&      family,
      pwr_options const&     options);

  void               reset(register_file& regs, reset_kind kind) override;
  void               on_event(std::uint32_t kind, std::uint64_t payload) override;
  [[nodiscard]] auto active_level() const -> std::uint32_t override;

  void vosr_written(std::uint32_t old, std::uint32_t now);
  void cr1_written(std::uint32_t old, std::uint32_t now);

  [[nodiscard]] auto secure_function() const -> bool override;
  [[nodiscard]] auto secure_word() const -> std::uint32_t override;
  [[nodiscard]] auto guard(std::uint32_t word, access_ctx const& ctx, bool write) -> bus_guard;

private:
  void               add_rules();
  [[nodiscard]] auto rule_secure(pwr_rule const& rule) const -> bool;

  pwr_options                options_;
  register_file&             regs_;
  pwr_family const&          family_;
  field_table<pwr_role>      names_;
  bool                       low_power_logged_{};
  reg_ref                    secure_;
  reg_ref                    privilege_;
  field_ref                  secure_privileged_;
  field_ref                  nonsecure_privileged_;
  std::optional<field_ref>   system_clock_;
  std::vector<pwr_rule>      rules_;
  std::optional<std::size_t> unit_;
  illegal_event              illegal_;
};

constexpr std::array<write_entry<pwr>, 3> u5_writes{
    {
     {"PWR_CR1", &pwr::cr1_written},
     {"PWR_VOSR", &pwr::vosr_written},
     {"PWR_SECCFGR", nullptr},
     }
};

constexpr auto u5_trustzone_writes = std::to_array<write_entry<pwr>>({
    {"PWR_CR2",      nullptr},
    {"PWR_CR3",      nullptr},
    {"PWR_SVMCR",    nullptr},
    {"PWR_WUCR1",    nullptr},
    {"PWR_WUCR2",    nullptr},
    {"PWR_WUCR3",    nullptr},
    {"PWR_BDCR1",    nullptr},
    {"PWR_BDCR2",    nullptr},
    {"PWR_DBPR",     nullptr},
    {"PWR_UCPDR",    nullptr},
    {"PWR_PRIVCFGR", nullptr},
    {"PWR_SR",       nullptr},
    {"PWR_WUSCR",    nullptr},
    {"PWR_APCR",     nullptr},
    {"PWR_PUCRA",    nullptr},
    {"PWR_PDCRA",    nullptr},
    {"PWR_PUCRB",    nullptr},
    {"PWR_PDCRB",    nullptr},
    {"PWR_PUCRC",    nullptr},
    {"PWR_PDCRC",    nullptr},
    {"PWR_PUCRD",    nullptr},
    {"PWR_PDCRD",    nullptr},
    {"PWR_PUCRE",    nullptr},
    {"PWR_PDCRE",    nullptr},
    {"PWR_PUCRF",    nullptr},
    {"PWR_PDCRF",    nullptr},
    {"PWR_PUCRG",    nullptr},
    {"PWR_PDCRG",    nullptr},
    {"PWR_PUCRH",    nullptr},
    {"PWR_PDCRH",    nullptr},
    {"PWR_PUCRI",    nullptr},
    {"PWR_PDCRI",    nullptr},
});

struct own_group
{
  std::string_view                bit;
  std::array<std::string_view, 3> registers;
};

constexpr std::array<own_group, 4> own_groups{
    {
     {"LPMSEC", {"PWR_CR1", "PWR_CR2", ""}},
     {"VDMSEC", {"PWR_CR3", "PWR_SVMCR", ""}},
     {"VBSEC", {"PWR_BDCR1", "PWR_BDCR2", "PWR_DBPR"}},
     {"APCSEC", {"PWR_APCR", "", ""}},
     }
};

/** @brief STM32U5 PWR, RM0456 10.10: VOS 00 range 4 to 11 range 1, ready bits read 1. */
constexpr std::array<named_field<pwr_role>, role_count<pwr_role>> u5_names{
    {
     {pwr_role::vos, {"PWR_VOSR", "VOS"}},
     {pwr_role::vos_ready, {"PWR_VOSR", "VOSRDY"}},
     {pwr_role::active_vos, {"PWR_SVMSR", "ACTVOS"}},
     {pwr_role::active_vos_ready, {"PWR_SVMSR", "ACTVOSRDY"}},
     {pwr_role::mode, {"PWR_CR1", "LPMS"}},
     {pwr_role::booster, {"PWR_VOSR", "BOOSTEN"}},
     {pwr_role::booster_ready, {"PWR_VOSR", "BOOSTRDY"}},
     }
};

constexpr std::array<std::uint8_t, 4> u5_vos_levels{3, 2, 1, 0};

/** @brief STM32H5 PWR, RM0481 10.11: VOS 00 VOS3 to 11 VOS0, VOSRDY and ACTVOSRDY read 1 when
 *         ready, no EPOD booster, LPMS 0 Stop and 1 Standby. */
constexpr std::array<named_field<pwr_role>, role_count<pwr_role>> h5_names{
    {
     {pwr_role::vos, {"PWR_VOSCR", "VOS"}},
     {pwr_role::vos_ready, {"PWR_VOSSR", "VOSRDY"}},
     {pwr_role::active_vos, {"PWR_VOSSR", "ACTVOS"}},
     {pwr_role::active_vos_ready, {"PWR_VOSSR", "ACTVOSRDY"}},
     {pwr_role::mode, {"PWR_PMCR", "LPMS"}},
     {pwr_role::booster, {"", ""}},
     {pwr_role::booster_ready, {"", ""}},
     }
};

constexpr std::array<std::uint8_t, 4> h5_vos_levels{3, 2, 1, 0};

constexpr std::array<write_entry<pwr>, 3> h5_writes{
    {
     {"PWR_PMCR", &pwr::cr1_written},
     {"PWR_VOSCR", &pwr::vosr_written},
     {"PWR_SECCFGR", nullptr},
     }
};

constexpr pwr_family h5_pwr{
    .fields           = h5_names,
    .modes            = h5_modes_by_lpms,
    .vos_levels       = h5_vos_levels,
    .vos_ready_set    = true,
    .active_ready_set = true,
    .writes           = h5_writes,
    .trustzone_writes = {},
};

constexpr pwr_family u5_pwr{
    .fields           = u5_names,
    .modes            = modes_by_lpms,
    .vos_levels       = u5_vos_levels,
    .vos_ready_set    = true,
    .active_ready_set = true,
    .writes           = u5_writes,
    .trustzone_writes = u5_trustzone_writes,
};

static_assert(names_every_role(u5_names));
static_assert(names_every_role(h5_names));

pwr::pwr(
    model_context&         ctx,
    peripheral_desc const& desc,
    pwr_family const&      family,
    pwr_options const&     options)
    : dispatch_model{ctx, desc, "PWR", family.writes, family.trustzone_writes}, options_{options},
      regs_{ctx.regs}, family_{family},
      names_{
          family.fields,
          [this](std::string_view reg_name, std::string_view name) {
            return field(reg_name, name);
          },
          [this](std::string_view reg_name, std::string_view name) {
            return has_field(reg_name, name);
          }},
      secure_{reg("PWR_SECCFGR")}, unit_{unit_named(ctx.part, desc.name)}
{
  if(ctx.trustzone)
  {
    privilege_            = reg("PWR_PRIVCFGR");
    secure_privileged_    = field("PWR_PRIVCFGR", "SPRIV");
    nonsecure_privileged_ = field("PWR_PRIVCFGR", "NSPRIV");
    add_rules();
  }
}

void pwr::add_rules()
{
  auto const whole = [this](std::string_view name) {
    auto const at = reg(name);
    return field_ref{.offset = at.offset, .lsb = at.lsb, .width = at.width};
  };
  for(auto const& group : own_groups)
  {
    auto const bit = field("PWR_SECCFGR", group.bit);
    for(auto const name : group.registers)
    {
      if(not name.empty())
      {
        auto const all = whole(name);
        rules_.push_back(
            {.offset = all.offset, .bits = all.mask(), .source = guard_source::own, .own = bit});
      }
    }
  }
  auto const css = field("PWR_SR", "CSSF");
  rules_.push_back(
      {.offset = css.offset,
       .bits   = css.mask(),
       .source = guard_source::own,
       .own    = field("PWR_SECCFGR", "LPMSEC")});
  for(int x = 1; x <= 8; ++x)
  {
    auto const n   = std::to_string(x);
    auto const bit = field("PWR_SECCFGR", "WUP" + n + "SEC");
    for(auto const& [name, prefix] : {
            std::pair{"PWR_WUCR1", "WUPEN"},
            std::pair{"PWR_WUCR2", "WUPP" },
            std::pair{"PWR_WUCR3", "WUSEL"},
            std::pair{"PWR_WUSCR", "CWUF" }
    })
    {
      auto const f = field(name, prefix + n);
      rules_.push_back(
          {.offset = f.offset, .bits = f.mask(), .source = guard_source::own, .own = bit});
    }
  }
  for(auto const name : {"VOS", "BOOSTEN", "USBPWREN", "USBBOOSTEN"})
  {
    auto const& vosr = describe("PWR_VOSR");
    if(std::ranges::find(vosr.fields, std::string_view{name}, &field_desc::name)
       != vosr.fields.end())
    {
      auto const f = field("PWR_VOSR", name);
      rules_.push_back(
          {.offset = f.offset, .bits = f.mask(), .source = guard_source::system_clock});
    }
  }
  auto const ucpd = whole("PWR_UCPDR");
  rules_.push_back(
      {.offset = ucpd.offset,
       .bits   = ucpd.mask(),
       .source = guard_source::peripheral,
       .unit   = unit_named(ctx_.part, "UCPD1")});
  for(char port = 'A'; port <= 'J'; ++port)
  {
    for(auto const& [kind, prefix] : {
            std::pair{"PUCR", "PU"},
            std::pair{"PDCR", "PD"}
    })
    {
      auto const name = std::format("PWR_{}{}", kind, port);
      if(not has_register(name))
      {
        continue;
      }
      for(auto const& bits : describe(name).fields)
      {
        auto const pin = std::string_view{bits.name}.substr(std::string_view{prefix}.size());
        auto const f   = field(name, bits.name);
        rules_.push_back(
            {.offset = f.offset,
             .bits   = f.mask(),
             .source = guard_source::port_pin,
             .peer   = std::format("GPIO{}", port),
             .pin    = static_cast<std::uint8_t>(std::stoi(std::string{pin}))});
      }
    }
  }
  auto const rcc =
      std::ranges::find(ctx_.part.peripherals, std::string_view{"RCC"}, &peripheral_desc::name);
  if(rcc != ctx_.part.peripherals.end())
  {
    if(auto const* const described = find_register(*rcc, "RCC_SECCFGR"))
    {
      auto const sysclk =
          std::ranges::find(described->fields, std::string_view{"SYSCLKSEC"}, &field_desc::name);
      if(sysclk != described->fields.end())
      {
        system_clock_ = field_ref{
            .offset = described->offset & ~3u,
            .lsb    = static_cast<std::uint8_t>(8 * (described->offset % 4) + sysclk->lsb),
            .width  = sysclk->width};
      }
    }
  }
}

auto pwr::rule_secure(pwr_rule const& rule) const -> bool
{
  switch(rule.source)
  {
    case guard_source::own:
      return rule.own.of(regs_.peek(rule.own.offset)) != 0;
    case guard_source::system_clock:
    {
      auto const* const rcc = dynamic_cast<trustzone_aware const*>(ctx_.peers.find("RCC"));
      return rcc != nullptr and system_clock_ and system_clock_->of(rcc->secure_word()) != 0;
    }
    case guard_source::port_pin:
    {
      auto const* const port = dynamic_cast<trustzone_aware const*>(ctx_.peers.find(rule.peer));
      return port != nullptr and (port->secure_word() >> rule.pin & 1u) != 0;
    }
    case guard_source::peripheral:
      return rule.unit and unit_secure(ctx_.peers, ctx_.part, *rule.unit);
  }
  return false;
}

auto pwr::secure_word() const -> std::uint32_t
{
  return ctx_.trustzone ? regs_.peek(secure_.offset) : 0;
}

auto pwr::secure_function() const -> bool
{
  return secure_word() != 0;
}

auto pwr::guard(std::uint32_t word, access_ctx const& ctx, bool write) -> bus_guard
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
    auto const spriv = secure_privileged_.of(regs_.peek(privilege_.offset)) != 0;
    return {.wi = not ctx.privileged and spriv ? all : 0};
  }
  if(not ctx_.trustzone)
  {
    return {};
  }
  auto const privilege = regs_.peek(privilege_.offset);
  if(word == privilege_.offset)
  {
    if(not ctx.privileged)
    {
      return {.wi = all};
    }
    return {.wi = ctx.secure ? 0 : secure_privileged_.mask()};
  }
  auto const spriv  = secure_privileged_.of(privilege) != 0;
  auto const nspriv = nonsecure_privileged_.of(privilege) != 0;
  bus_guard  out;
  for(auto const& rule : rules_)
  {
    if(rule.offset != word)
    {
      continue;
    }
    auto const secure = rule_secure(rule);
    out               = out | guard_of(rule.bits, secure, secure ? spriv : nspriv, ctx);
  }
  return out;
}

void pwr::reset(register_file& regs, reset_kind)
{
  for(auto const& name : claims())
  {
    regs.poke(reg(name).offset, describe(name).reset);
  }
}

void pwr::vosr_written(std::uint32_t old, std::uint32_t now)
{
  auto const level = names_[pwr_role::vos];
  if(level.of(now) != level.of(old))
  {
    if(family_.vos_levels[level.of(now)] < family_.vos_levels[level.of(old)])
    {
      update(regs_, names_[pwr_role::vos_ready], family_.vos_ready_set ? 0 : 1);
    }
    update(regs_, names_[pwr_role::active_vos_ready], family_.active_ready_set ? 0 : 1);
    schedule_in(options_.ready_delay, voltage_ready, ++state_.generation[voltage_ready]);
  }
  auto const booster = names_.find(pwr_role::booster);
  if(booster and booster->of(now) != booster->of(old))
  {
    update(regs_, names_[pwr_role::booster_ready], 0);
    auto const generation = ++state_.generation[booster_ready];
    if(booster->of(now) != 0)
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
      family_.fields[std::to_underlying(pwr_role::mode)].name.reg,
      std::format(
          "LPMS selects {}, not modelled: a deep-sleep WFI idles as a plain WFI",
          reflex::enum_value_name(family_.modes[names_[pwr_role::mode].of(now)])));
}

auto pwr::active_level() const -> std::uint32_t
{
  return family_.vos_levels[get(regs_, names_[pwr_role::active_vos])];
}

void pwr::on_event(std::uint32_t kind, std::uint64_t payload)
{
  if(kind > booster_ready or payload != state_.generation[kind])
  {
    return;
  }
  if(kind == voltage_ready)
  {
    update(regs_, names_[pwr_role::vos_ready], family_.vos_ready_set ? 1 : 0);
    update(regs_, names_[pwr_role::active_vos], get(regs_, names_[pwr_role::vos]));
    update(regs_, names_[pwr_role::active_vos_ready], family_.active_ready_set ? 1 : 0);
    for(auto* const peer : ctx_.peers.all())
    {
      if(auto* const listener = dynamic_cast<voltage_range_listener*>(peer))
      {
        listener->on_voltage_range_change();
      }
    }
  }
  else if(
      auto const booster = names_.find(pwr_role::booster); booster and get(regs_, *booster) != 0)
  {
    update(regs_, names_[pwr_role::booster_ready], 1);
  }
}
}

auto pwr_factory(pwr_options options) -> model_factory
{
  return [options](model_context& ctx, peripheral_desc const& desc) -> std::shared_ptr<model> {
    return std::make_shared<pwr>(ctx, desc, u5_pwr, options);
  };
}

auto pwr_h5_factory(pwr_options options) -> model_factory
{
  return [options](model_context& ctx, peripheral_desc const& desc) -> std::shared_ptr<model> {
    return std::make_shared<pwr>(ctx, desc, h5_pwr, options);
  };
}
}
