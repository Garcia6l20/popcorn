#include "gtzc.hpp"

#include "factories.hpp"
#include "model_base.hpp"

#include <popcorn/periph/firewall.hpp>
#include <popcorn/periph/stm32/board.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

namespace popcorn::periph::stm32
{
namespace
{
constexpr std::array<std::string_view, 2> memory_stems{"FLASH", "BKPSRAM"};

struct watermark_memory
{
  std::string_view registers;
  std::string_view region;
  std::string_view enable_field;
  std::uint32_t    granule;
};

constexpr std::array<watermark_memory, 1> watermark_memories{
    {{"MPCWM4", "BKPSRAM", "BKPSRAMIE", 32}}};

constexpr std::string_view gtzc_irq_name = "GTZC_IRQn";

auto all_digits(std::string_view text) -> bool
{
  return not text.empty()
     and std::ranges::all_of(text, [](char c) { return c >= '0' and c <= '9'; });
}

auto memory_stem(std::string_view stem) -> bool
{
  return std::ranges::contains(memory_stems, stem)
      or stem.ends_with("_MEM")
      or (stem.starts_with("SRAM") and all_digits(stem.substr(4)));
}
}

auto unit_named(part_description const& part, std::string_view name) -> std::optional<std::size_t>
{
  auto const found = std::ranges::find_if(part.peripherals, [&](peripheral_desc const& p) {
    return p.name == name and not p.alias_of;
  });
  if(found == part.peripherals.end())
  {
    return std::nullopt;
  }
  return static_cast<std::size_t>(found - part.peripherals.begin());
}

namespace
{
auto unit_of_stem(part_description const& part, gtzc_variant variant, std::string_view stem)
    -> std::optional<std::size_t>
{
  if(auto const renamed = std::ranges::find(variant.renames, stem, &gtzc_rename::stem);
     renamed != variant.renames.end())
  {
    return unit_named(part, renamed->peripheral);
  }
  for(std::string_view const block : {"TZSC", "TZIC"})
  {
    if(stem.starts_with(block) and all_digits(stem.substr(block.size())))
    {
      return unit_named(part, std::format("GTZC{}_{}", stem.substr(block.size()), block));
    }
  }
  if(stem.starts_with("MPCBB") and stem.ends_with("_REG"))
  {
    auto const suffix = std::format("_{}", stem.substr(0, stem.size() - 4));
    auto const found  = std::ranges::find_if(part.peripherals, [&](peripheral_desc const& p) {
      return p.ip == "GTZC" and not p.alias_of and p.name.ends_with(suffix);
    });
    if(found != part.peripherals.end())
    {
      return static_cast<std::size_t>(found - part.peripherals.begin());
    }
    return std::nullopt;
  }
  return unit_named(part, stem);
}

auto units_of_stem(part_description const& part, gtzc_variant variant, std::string_view stem)
    -> std::vector<std::size_t>
{
  std::vector<std::size_t> out;
  if(memory_stem(stem))
  {
    return out;
  }
  if(auto const primary = unit_of_stem(part, variant, stem))
  {
    out.push_back(*primary);
    for(auto const& tie : variant.companions)
    {
      if(tie.primary == part.peripherals[*primary].name)
      {
        if(auto const unit = unit_named(part, tie.also))
        {
          out.push_back(*unit);
        }
      }
    }
  }
  return out;
}

auto field_of(register_desc const& reg, field_desc const& field) -> field_ref
{
  return {
      .offset = reg.offset & ~3u,
      .lsb    = static_cast<std::uint8_t>(8 * (reg.offset % 4) + field.lsb),
      .width  = field.width};
}

auto fields_mask(register_desc const& reg) -> std::uint32_t
{
  std::uint32_t mask = 0;
  for(auto const& field : reg.fields)
  {
    mask |= field_of(reg, field).mask();
  }
  return mask;
}

auto numbered(std::string_view name, std::string_view prefix) -> std::optional<std::uint32_t>
{
  if(not name.starts_with(prefix) or not all_digits(name.substr(prefix.size())))
  {
    return std::nullopt;
  }
  std::uint32_t value = 0;
  auto const    rest  = name.substr(prefix.size());
  std::from_chars(rest.data(), rest.data() + rest.size(), value);
  return value;
}

auto is_tzsc(peripheral_desc const& p) -> bool
{
  return p.ip == "GTZC" and not p.alias_of and p.name.ends_with("_TZSC");
}

auto is_tzic(peripheral_desc const& p) -> bool
{
  return p.ip == "GTZC" and not p.alias_of and p.name.ends_with("_TZIC");
}

auto gtzc_prefix(std::string_view name) -> std::string_view
{
  return name.substr(0, name.rfind('_'));
}

enum class memory_kind : std::uint8_t
{
  sram,
  flash,
  watermark,
};

struct memory_window
{
  address                       start{};
  std::uint32_t                 size{};
  address                       physical{};
  memory_kind                   kind{};
  sram_block_controller const*  blocks{};
  secure_page_controller const* pages{};
  std::uint32_t                 granule{};
  illegal_access_controller*    events{};
  illegal_source                source{};
};

struct memory_rule
{
  bool secure{};
  bool privileged{};
  bool secure_data{};
  bool hidden{};
};

struct data_decision
{
  bus_verdict verdict{};
  bool        event{};
};

[[gnu::hot]] auto decide_data(memory_rule rule, bus_attrs attrs) -> data_decision
{
  if(attrs.secure and not rule.secure and not rule.secure_data)
  {
    return {.verdict = bus_verdict::read_zero_write_ignore, .event = true};
  }
  if(not attrs.secure and rule.secure)
  {
    return {.verdict = bus_verdict::read_zero_write_ignore, .event = true};
  }
  if(rule.hidden or (not attrs.privileged and rule.privileged))
  {
    return {.verdict = bus_verdict::read_zero_write_ignore};
  }
  return {.verdict = bus_verdict::allow};
}

auto fetch_denied(memory_rule rule, bus_attrs attrs) -> bool
{
  return attrs.secure != rule.secure or rule.hidden or (not attrs.privileged and rule.privileged);
}

struct gtzc_state
{
  friend auto operator==(gtzc_state const&, gtzc_state const&) -> bool = default;
};

/**
 * @brief Base of the GTZC block models: the register claims, the block's own illegal access
 *        event, and the GTZC_IRQn line when the description gives the IRQ to this block.
 */
class gtzc_block : public stm32_model<gtzc_state>
{
public:
  [[nodiscard]] auto claims() const -> std::vector<std::string> override
  {
    return claims_;
  }

  void on_event(std::uint32_t, std::uint64_t) override
  {}

  [[nodiscard]] auto gtzc_line() const noexcept -> irq_line*
  {
    return line_;
  }

  [[nodiscard]] auto registers() const noexcept -> register_file const&
  {
    return ctx_.regs;
  }

protected:
  gtzc_block(model_context& ctx, peripheral_desc const& desc, std::string_view model_name)
      : stm32_model{ctx, desc, model_name}, unit_{*unit_named(ctx.part, desc.name)}
  {
    auto const named = std::ranges::find(ctx.part.irqs, gtzc_irq_name, &irq_desc::name);
    if(named != ctx.part.irqs.end())
    {
      auto const at = std::ranges::find(desc.irqs, named->number);
      if(at != desc.irqs.end())
      {
        line_ = &ctx.irqs[static_cast<std::size_t>(at - desc.irqs.begin())];
      }
    }
  }

  void claim(std::string_view name)
  {
    claims_.emplace_back(describe(name).name);
  }

  void own_illegal_access(initiator by)
  {
    if(by == initiator::debugger)
    {
      return;
    }
    if(not own_linked_ and not ctx_.peers.all().empty())
    {
      own_linked_ = true;
      for(auto* const peer : ctx_.peers.all())
      {
        if(auto* const tzic = dynamic_cast<illegal_access_controller*>(peer))
        {
          if(auto const source = tzic->source_of(unit_))
          {
            own_tzic_   = tzic;
            own_source_ = *source;
            break;
          }
        }
      }
    }
    if(own_tzic_ != nullptr)
    {
      own_tzic_->illegal_access(own_source_);
    }
  }

  [[nodiscard]] auto unit() const noexcept -> std::size_t
  {
    return unit_;
  }

private:
  std::vector<std::string>   claims_;
  std::size_t                unit_;
  irq_line*                  line_{};
  bool                       own_linked_{};
  illegal_access_controller* own_tzic_{};
  illegal_source             own_source_{};
};

enum class tzsc_role : std::uint8_t
{
  control,
  security,
  privilege,
  watermark_config,
  watermark_area,
};

struct watermark_regs
{
  std::uint32_t    config{};
  std::uint32_t    area{};
  field_ref        enable;
  field_ref        lock;
  field_ref        secure;
  field_ref        privileged;
  field_ref        first;
  field_ref        length;
  std::string_view region;
  std::string_view enable_field;
  std::uint32_t    granule{};
};

struct tzsc_word
{
  std::uint32_t offset{};
  tzsc_role     role{};
  std::uint32_t writable{};
  std::uint32_t pair{};
};

struct securable
{
  std::uint8_t               owner{};
  field_ref                  sec;
  field_ref                  priv;
  std::uint8_t               tzic{};
  illegal_access_controller* events{};
  illegal_source             source{};
  bool                       present{};
};

/**
 * @brief TZSC of an STM32 GTZC under TrustZone, RM0456 5.4.1, 5.4.3 and 5.6. `CR` is Secure
 *        privileged only and `CR.LCK` locks `SECCFGRx` and `PRIVCFGRx` until the next reset.
 *        `SECCFGRx` and `PRIVCFGRx` read for every transaction. A `SECCFGRx` bit changes only
 *        with a Secure transaction, privileged when the peripheral's `PRIVCFGRx` bit is set. A
 *        `PRIVCFGRx` bit changes only with a privileged transaction, Secure when the
 *        peripheral's `SECCFGRx` bit is set. A Non-secure write to `CR` or `SECCFGRx` and a
 *        Non-secure read of `CR` raise the TZSC's own TZIC event, RM0456 3.5.5. The MPCWM
 *        registers stay storage, except MPCWM4 of the BKPSRAM, RM0456 5.6.8 and 5.6.9.
 *
 *        The first TZSC of the part in description order is the machine's bus firewall for
 *        the securable peripherals of every TZSC, each from its `<NAME>SEC` and `<NAME>PRIV`
 *        fields. A Non-secure data access to a Secure peripheral reads zero and drops writes,
 *        with an event in the TZIC of the peripheral's GTZC, RM0456 5.4.2 for writes and the
 *        TrustZone-aware rule of RM0456 3.5.5 for reads. An unprivileged data access to a
 *        privileged peripheral reads zero and drops writes, with no event. No access is a bus
 *        error: a fetch never reaches a peripheral window.
 *
 *        One bit covers two description peripherals where RM0456 or the layout gives it two
 *        blocks: `DCMISEC` also PSSI and `ADC1SEC` also the ADC12 common registers, RM0456
 *        5.6.4. `FDCAN1SEC` also FDCAN1_RAM, `SDMMCnSEC` also DLYBSDn and `OCTOSPInSEC` also
 *        DLYBOSn are inferred from the memory layout: RM0456 Table 4 and the ST HAL list no bit
 *        for these blocks, which have a Secure alias. The same ties hold for their TZIC events.
 *        A user model in place of this firewall's TZSC turns off the checks of every TZSC of
 *        the part, privilege included.
 *
 *        The firewall also filters memory, RM0456 5.4.2: the SRAM blocks of every MPCBB, the
 *        flash pages of the FLASH model and the BKPSRAM through MPCWM4. A Non-secure data access
 *        to a Secure block, page or sub-region, or a Secure one to a Non-secure block unless the
 *        MPCBB's `CR.SRWILADIS` is set, reads zero, drops the write and raises the memory's
 *        TZIC event, `SRAMnIE` with n the MPCBB's digit, `FLASHIE` or `BKPSRAMIE`, RM0456 Table
 *        68 for flash. A write to flash passes to the FLASH model, which programs flash and
 *        applies the write column of Table 68 itself. An unprivileged data access to a
 *        privileged block or sub-region reads zero and drops the write with no event. An
 *        instruction fetch the same rules refuse, a Secure fetch with `SRWILADIS` set included,
 *        is a BusFault, with the event for a Secure fetch of a Non-secure SRAM block or BKPSRAM
 *        sub-region only. A flash page in an HDP area whose access is disabled refuses every data
 *        access and fetch, with the event only for a Non-secure one. MPCWM4 registers are
 *        Secure privileged only, `SRLOCK` set-only freezes both until a reset, a sub-region of
 *        length 0 clears `SREN`, and outside an enabled sub-region the BKPSRAM is Secure and
 *        privileged. Every write that changes a security or privilege bit, here or in an MPCBB,
 *        drops the engine's page verdicts and ends the engine run after the instruction, a
 *        `SRWILADIS` change drops the verdicts only, and a write that changes nothing does
 *        neither.
 *
 *        A filtered debugger access, a `m.mem.secure` or `m.mem.nonsecure` view, is decided
 *        as a CPU access of its attributes, and a write to flash follows the read rule of its
 *        page since no FLASH model programs it. Deviation: no debugger access raises a TZIC
 *        event, here, in the MPCBBs or in a TrustZone-aware model, so a view leaves the
 *        firmware's TZIC flags and GTZC_IRQn as they are.
 */
class tzsc final : public gtzc_block, public bus_firewall, public peripheral_security
{
public:
  tzsc(model_context& ctx, peripheral_desc const& desc, gtzc_variant variant)
      : gtzc_block{ctx, desc, "GTZC TZSC"}, variant_{variant}
  {
    claim("CR");
    lock_ = field("CR", "LCK");
    words_.push_back(
        {.offset = reg("CR").offset, .role = tzsc_role::control, .writable = lock_.mask()});
    for(auto const& described : desc.registers)
    {
      auto const index = numbered(described.name, "SECCFGR");
      if(not index)
      {
        continue;
      }
      auto const  pair_name = std::format("PRIVCFGR{}", *index);
      auto const& paired    = describe(pair_name);
      claim(described.name);
      claim(pair_name);
      words_.push_back(
          {.offset   = described.offset & ~3u,
           .role     = tzsc_role::security,
           .writable = fields_mask(described),
           .pair     = paired.offset & ~3u});
      words_.push_back(
          {.offset   = paired.offset & ~3u,
           .role     = tzsc_role::privilege,
           .writable = fields_mask(paired),
           .pair     = described.offset & ~3u});
    }
    auto const first = std::ranges::find_if(ctx.part.peripherals, is_tzsc);
    if(first != ctx.part.peripherals.end() and first->name == desc.name)
    {
      claim_watermark();
      build_rules();
      ctx.install_firewall(*this);
    }
  }

  void reset(register_file& regs, reset_kind) override
  {
    for(auto const& name : claims())
    {
      regs.poke(reg(name).offset, describe(name).reset);
    }
    resolve();
    resolve_memory();
    ctx_.services.memory_policy_changed();
  }

  auto on_read(register_file& regs, std::uint32_t offset, std::uint8_t size, access_ctx ctx)
      -> std::expected<std::uint32_t, bus_error> override
  {
    auto const* const word = word_at(offset);
    if(word->role == tzsc_role::control
       or word->role == tzsc_role::watermark_config
       or word->role == tzsc_role::watermark_area)
    {
      if(not ctx.secure)
      {
        own_illegal_access(ctx.by);
        return 0;
      }
      if(not ctx.privileged)
      {
        return 0;
      }
    }
    return read_lanes(regs.peek(word->offset) & word->writable, offset, size);
  }

  auto on_write(
      register_file& regs,
      std::uint32_t  offset,
      std::uint8_t   size,
      std::uint32_t,
      std::uint32_t value,
      access_ctx    ctx) -> std::expected<void, bus_error> override
  {
    auto const* const word   = word_at(offset);
    auto const        old    = regs.peek(word->offset);
    auto const        merged = merge_write(old, offset, size, value);
    auto const        locked = lock_.of(regs.peek(lock_.offset)) != 0;
    switch(word->role)
    {
      case tzsc_role::control:
        if(not ctx.secure)
        {
          own_illegal_access(ctx.by);
          return {};
        }
        if(ctx.privileged)
        {
          regs.poke(word->offset, old | (merged & word->writable));
        }
        return {};
      case tzsc_role::security:
        if(not ctx.secure)
        {
          own_illegal_access(ctx.by);
          return {};
        }
        if(not locked)
        {
          auto const allowed = word->writable & (ctx.privileged ? ~0u : ~regs.peek(word->pair));
          regs.poke(word->offset, (merged & allowed) | (old & ~allowed));
        }
        return {};
      case tzsc_role::privilege:
        if(ctx.privileged and not locked)
        {
          auto const allowed = word->writable & (ctx.secure ? ~0u : ~regs.peek(word->pair));
          regs.poke(word->offset, (merged & allowed) | (old & ~allowed));
        }
        return {};
      case tzsc_role::watermark_config:
      case tzsc_role::watermark_area:
        if(not ctx.secure)
        {
          own_illegal_access(ctx.by);
          return {};
        }
        if(ctx.privileged)
        {
          watermark_written(regs, *word, merged);
        }
        return {};
    }
    return {};
  }

  [[nodiscard]] auto filters_memory() const -> bool override
  {
    return true;
  }

  [[gnu::hot, nodiscard]] auto
      memory_page(address page, std::uint32_t size, address at, bus_attrs attrs, arch::memory_op op)
          -> page_verdict override
  {
    resolve_memory();
    auto const* const window = window_at(at);
    if(window == nullptr)
    {
      return {};
    }
    page_verdict        verdict;
    std::optional<bool> first_denied;
    auto const          step = std::min(window->granule, size);
    for(auto part = page; part - page < size; part += step)
    {
      if(part < window->start or part - window->start >= window->size)
      {
        continue;
      }
      auto const rule = rule_at(*window, part);
      verdict.filter_data =
          verdict.filter_data or decide_data(rule, attrs).verdict != bus_verdict::allow;
      auto const denied  = fetch_denied(rule, attrs);
      verdict.fetch_each = verdict.fetch_each or (first_denied and *first_denied != denied);
      first_denied       = denied;
    }
    auto const rule    = rule_at(*window, at);
    verdict.deny_fetch = fetch_denied(rule, attrs);
    if(verdict.fetch_each and op == arch::memory_op::fetch)
    {
      verdict.fetch_window_shift = fetch_window_shift(page, size, at, step, attrs);
    }
    if(verdict.deny_fetch
       and op == arch::memory_op::fetch
       and attrs.secure
       and not rule.secure
       and window->kind != memory_kind::flash
       and window->events != nullptr)
    {
      window->events->illegal_access(window->source);
    }
    return verdict;
  }

  [[nodiscard]] auto check_memory(address at, std::uint8_t, bus_attrs attrs, arch::memory_op op)
      -> bus_verdict override
  {
    resolve_memory();
    auto const* const window = window_at(at);
    if(window == nullptr
       or (window->kind == memory_kind::flash
           and op == arch::memory_op::write
           and attrs.by != initiator::debugger))
    {
      return bus_verdict::allow;
    }
    auto const decision = decide_data(rule_at(*window, at), attrs);
    if(decision.event and window->events != nullptr and attrs.by != initiator::debugger)
    {
      window->events->illegal_access(window->source);
    }
    return decision.verdict;
  }

  [[nodiscard]] auto
      allowed_extent(address at, std::uint8_t size, bus_attrs attrs, arch::memory_op op)
          -> memory_extent override
  {
    auto const* const window = window_at(at);
    if(window == nullptr)
    {
      std::uint64_t first = 0;
      std::uint64_t last  = ~address{};
      for(auto const& w : windows_)
      {
        auto const end = std::uint64_t{w.start} + w.size;
        if(end <= at)
        {
          first = std::max(first, end);
        }
        else if(w.start > at)
        {
          last = std::min<std::uint64_t>(last, w.start - 1u);
        }
      }
      return {.first = static_cast<address>(first), .last = static_cast<address>(last)};
    }
    auto const window_last = static_cast<address>(window->start + (window->size - 1u));
    if(window->kind == memory_kind::flash
       and op == arch::memory_op::write
       and attrs.by != initiator::debugger)
    {
      return {.first = window->start, .last = window_last};
    }
    auto const granule = window->granule;
    if(granule == 0 or (granule & (granule - 1u)) != 0)
    {
      return {.first = at, .last = static_cast<address>(at + size - 1u)};
    }
    auto const physical = window->physical + (at - window->start);
    auto const unit     = std::uint64_t{at} - (physical & (granule - 1u));
    return {
        .first = static_cast<address>(std::max<std::uint64_t>(window->start, unit)),
        .last  = static_cast<address>(std::min<std::uint64_t>(window_last, unit + granule - 1u))};
  }

  [[nodiscard]] auto check_peripheral(std::size_t unit, bus_attrs attrs, arch::memory_op)
      -> bus_verdict override
  {
    if(not resolved_)
    {
      resolve();
    }
    if(unit >= rules_.size() or not rules_[unit].present)
    {
      return bus_verdict::allow;
    }
    auto const& rule = rules_[unit];
    auto const* regs = owners_[rule.owner];
    if(regs == nullptr)
    {
      return bus_verdict::allow;
    }
    if(not attrs.secure and rule.sec.of(regs->peek(rule.sec.offset)) != 0)
    {
      if(rule.events != nullptr and attrs.by != initiator::debugger)
      {
        rule.events->illegal_access(rule.source);
      }
      return bus_verdict::read_zero_write_ignore;
    }
    if(not attrs.privileged and rule.priv.of(regs->peek(rule.priv.offset)) != 0)
    {
      return bus_verdict::read_zero_write_ignore;
    }
    return bus_verdict::allow;
  }

  [[nodiscard]] auto peripheral_secure(std::size_t unit) -> std::optional<bool> override
  {
    if(not resolved_)
    {
      resolve();
    }
    if(unit >= rules_.size() or not rules_[unit].present)
    {
      return std::nullopt;
    }
    auto const& rule = rules_[unit];
    auto const* regs = owners_[rule.owner];
    if(regs == nullptr)
    {
      return std::nullopt;
    }
    return rule.sec.of(regs->peek(rule.sec.offset)) != 0;
  }

private:
  [[nodiscard]] auto word_at(std::uint32_t offset) const -> tzsc_word const*
  {
    return &*std::ranges::find(words_, offset & ~3u, &tzsc_word::offset);
  }

  void claim_watermark()
  {
    for(auto const& memory : watermark_memories)
    {
      auto const named = [&](std::string_view suffix) -> register_desc const* {
        auto const wanted = std::format("{}{}", memory.registers, suffix);
        auto const found  = std::ranges::find_if(
            desc_.registers, [&](register_desc const& r) { return r.name.ends_with(wanted); });
        return found != desc_.registers.end() ? &*found : nullptr;
      };
      auto const* const config = named("ACFGR");
      auto const* const area   = named("AR");
      auto const        mapped = std::ranges::any_of(ctx_.part.regions, [&](region_desc const& r) {
        return r.name.starts_with(memory.region);
      });
      if(config == nullptr or area == nullptr or not mapped)
      {
        continue;
      }
      claim(config->name);
      claim(area->name);
      watermark_ = watermark_regs{
          .config       = config->offset & ~3u,
          .area         = area->offset & ~3u,
          .enable       = field(config->name, "SREN"),
          .lock         = field(config->name, "SRLOCK"),
          .secure       = field(config->name, "SEC"),
          .privileged   = field(config->name, "PRIV"),
          .first        = field(area->name, "SUBA_START"),
          .length       = field(area->name, "SUBA_LENGTH"),
          .region       = memory.region,
          .enable_field = memory.enable_field,
          .granule      = memory.granule};
      words_.push_back(
          {.offset   = watermark_->config,
           .role     = tzsc_role::watermark_config,
           .writable = fields_mask(*config)});
      words_.push_back(
          {.offset   = watermark_->area,
           .role     = tzsc_role::watermark_area,
           .writable = fields_mask(*area)});
      return;
    }
  }

  void watermark_written(register_file& regs, tzsc_word const& word, std::uint32_t merged)
  {
    auto const& w      = *watermark_;
    auto const  config = regs.peek(w.config);
    auto const  area   = regs.peek(w.area);
    if(w.lock.of(config) != 0)
    {
      return;
    }
    auto next_config = config;
    auto next_area   = area;
    if(word.role == tzsc_role::watermark_config)
    {
      next_config = merged & word.writable;
    }
    else
    {
      next_area = merged & word.writable;
    }
    if(w.length.of(next_area) == 0)
    {
      next_config &= ~w.enable.mask();
    }
    regs.poke(w.config, next_config);
    regs.poke(w.area, next_area);
    auto const policy = w.enable.mask() | w.secure.mask() | w.privileged.mask();
    if((config & policy) != (next_config & policy) or area != next_area)
    {
      ctx_.services.memory_policy_changed();
      ctx_.services.end_after_instruction();
    }
  }

  [[nodiscard]] auto watermark_rule(address offset) const -> memory_rule
  {
    auto const& w      = *watermark_;
    auto const  config = ctx_.regs.peek(w.config);
    auto const  area   = ctx_.regs.peek(w.area);
    auto const  first  = w.first.of(area);
    auto const  length = w.length.of(area);
    auto const  at     = offset / w.granule;
    if(w.enable.of(config) != 0 and length != 0 and at >= first and at - first < length)
    {
      return {.secure = w.secure.of(config) != 0, .privileged = w.privileged.of(config) != 0};
    }
    return {.secure = true, .privileged = true};
  }

  [[gnu::hot, nodiscard]] auto rule_at(memory_window const& window, address at) const -> memory_rule
  {
    auto const physical = window.physical + (at - window.start);
    switch(window.kind)
    {
      case memory_kind::sram:
        return {
            .secure      = window.blocks->block_secure(physical).value_or(true),
            .privileged  = window.blocks->block_privileged(physical).value_or(true),
            .secure_data = window.blocks->secure_data_allowed()};
      case memory_kind::flash:
      {
        auto const page = window.pages->page_rule(at).value_or(page_policy{});
        return {.secure = page.secure, .privileged = page.privileged, .hidden = page.hidden};
      }
      case memory_kind::watermark:
        return watermark_rule(at - window.start);
    }
    return {};
  }

  [[nodiscard]] auto fetch_denied_at(address at, bus_attrs attrs) const -> bool
  {
    auto const* const window = window_at(at);
    return window != nullptr and fetch_denied(rule_at(*window, at), attrs);
  }

  [[nodiscard]] auto fetch_window_shift(
      address       page,
      std::uint32_t size,
      address       at,
      std::uint32_t step,
      bus_attrs     attrs) const -> std::uint8_t
  {
    auto const denied = fetch_denied_at(at, attrs);
    auto       shift  = std::countr_zero(step);
    for(auto span = step; span < size; span *= 2)
    {
      auto const block   = page + ((at - page) & ~(2 * span - 1));
      auto const sibling = block + ((at - block) < span ? span : 0);
      for(auto part = sibling; part < sibling + span; part += step)
      {
        if(fetch_denied_at(part, attrs) != denied)
        {
          return static_cast<std::uint8_t>(shift);
        }
      }
      ++shift;
    }
    return static_cast<std::uint8_t>(shift);
  }

  [[nodiscard]] auto window_at(address at) const -> memory_window const*
  {
    auto const found = std::ranges::find_if(
        windows_, [at](memory_window const& w) { return at >= w.start and at - w.start < w.size; });
    return found != windows_.end() ? &*found : nullptr;
  }

  void resolve_memory()
  {
    if(memory_resolved_ or rules_.empty() or ctx_.peers.all().empty())
    {
      return;
    }
    memory_resolved_ = true;
    std::vector<illegal_access_controller*>   tzics;
    std::vector<sram_block_controller const*> srams;
    secure_page_controller const*             pages{};
    for(auto* const peer : ctx_.peers.all())
    {
      if(auto* const tzic = dynamic_cast<illegal_access_controller*>(peer))
      {
        tzics.push_back(tzic);
      }
      if(auto const* const sram = dynamic_cast<sram_block_controller const*>(peer))
      {
        srams.push_back(sram);
      }
      if(auto const* const flash = dynamic_cast<secure_page_controller const*>(peer))
      {
        pages = flash;
      }
    }
    auto const event_of = [&](std::string_view field) {
      for(auto* const tzic : tzics)
      {
        if(auto const source = tzic->source(field))
        {
          return std::pair{tzic, *source};
        }
      }
      return std::pair<illegal_access_controller*, illegal_source>{nullptr, {}};
    };
    for(auto const& region : ctx_.part.regions)
    {
      auto const physical = region.physical.value_or(region.start);
      auto const end      = std::uint64_t{physical} + region.size;
      for(auto const* const sram : srams)
      {
        auto const blocks = sram->blocks();
        if(not blocks)
        {
          continue;
        }
        auto const low  = std::max<std::uint64_t>(physical, blocks->start);
        auto const high = std::min<std::uint64_t>(end, std::uint64_t{blocks->start} + blocks->size);
        if(low >= high)
        {
          continue;
        }
        auto const digit = blocks->controller.substr(blocks->controller.rfind("MPCBB") + 5);
        auto const [events, source] = event_of(std::format("SRAM{}IE", digit));
        windows_.push_back(
            {.start    = static_cast<address>(region.start + (low - physical)),
             .size     = static_cast<std::uint32_t>(high - low),
             .physical = static_cast<address>(low),
             .kind     = memory_kind::sram,
             .blocks   = sram,
             .granule  = blocks->block_size,
             .events   = events,
             .source   = source});
      }
      if(pages != nullptr and pages->page_secure(region.start))
      {
        auto const [events, source] = event_of("FLASHIE");
        windows_.push_back(
            {.start    = region.start,
             .size     = region.size,
             .physical = physical,
             .kind     = memory_kind::flash,
             .pages    = pages,
             .granule  = pages->secure_page_size(),
             .events   = events,
             .source   = source});
      }
      if(watermark_ and region.name.starts_with(watermark_->region))
      {
        auto const [events, source] = event_of(watermark_->enable_field);
        windows_.push_back(
            {.start    = region.start,
             .size     = region.size,
             .physical = physical,
             .kind     = memory_kind::watermark,
             .granule  = watermark_->granule,
             .events   = events,
             .source   = source});
      }
    }
  }

  void build_rules()
  {
    auto const& part = ctx_.part;
    rules_.resize(part.peripherals.size());
    for(auto const& unit : part.peripherals)
    {
      if(not is_tzsc(unit))
      {
        continue;
      }
      auto const owner = static_cast<std::uint8_t>(tzsc_names_.size());
      tzsc_names_.emplace_back(unit.name);
      tzic_names_.push_back(std::format("{}_TZIC", gtzc_prefix(unit.name)));
      for(auto const& described : unit.registers)
      {
        auto const index = numbered(described.name, "SECCFGR");
        if(not index)
        {
          continue;
        }
        auto const* const paired = find_register(unit, std::format("PRIVCFGR{}", *index));
        for(auto const& sec : described.fields)
        {
          if(not sec.name.ends_with("SEC"))
          {
            continue;
          }
          auto const stem = std::string_view{sec.name}.substr(0, sec.name.size() - 3);
          field_ref  priv_bit{};
          if(paired != nullptr)
          {
            auto const priv =
                std::ranges::find(paired->fields, std::format("{}PRIV", stem), &field_desc::name);
            if(priv != paired->fields.end())
            {
              priv_bit = field_of(*paired, *priv);
            }
          }
          for(auto const target : units_of_stem(part, variant_, stem))
          {
            auto& rule   = rules_[target];
            rule.present = true;
            rule.owner   = owner;
            rule.tzic    = owner;
            rule.sec     = field_of(described, sec);
            rule.priv    = priv_bit;
          }
        }
      }
    }
    owners_.assign(tzsc_names_.size(), nullptr);
  }

  void resolve()
  {
    if(resolved_ or rules_.empty() or ctx_.peers.all().empty())
    {
      return;
    }
    resolved_ = true;
    std::vector<illegal_access_controller*> tzics;
    for(auto const& [index, name] : std::views::enumerate(tzsc_names_))
    {
      auto* const peer                         = dynamic_cast<gtzc_block*>(ctx_.peers.find(name));
      owners_[static_cast<std::size_t>(index)] = peer != nullptr ? &peer->registers() : nullptr;
      tzics.push_back(
          dynamic_cast<illegal_access_controller*>(
              ctx_.peers.find(tzic_names_[static_cast<std::size_t>(index)])));
    }
    for(auto const& [unit, rule] : std::views::enumerate(rules_))
    {
      if(not rule.present or tzics[rule.tzic] == nullptr)
      {
        continue;
      }
      if(auto const source = tzics[rule.tzic]->source_of(static_cast<std::size_t>(unit)))
      {
        rule.events = tzics[rule.tzic];
        rule.source = *source;
      }
    }
  }

  gtzc_variant                      variant_;
  field_ref                         lock_;
  std::vector<tzsc_word>            words_;
  std::vector<securable>            rules_;
  std::vector<std::string>          tzsc_names_;
  std::vector<std::string>          tzic_names_;
  std::vector<register_file const*> owners_;
  bool                              resolved_{};
  std::optional<watermark_regs>     watermark_;
  std::vector<memory_window>        windows_;
  bool                              memory_resolved_{};
};

struct tzic_source
{
  std::string              field;
  illegal_source           at;
  std::vector<std::size_t> units;
};

enum class tzic_role : std::uint8_t
{
  enable,
  status,
  clear,
};

struct tzic_word
{
  std::uint32_t offset{};
  tzic_role     role{};
  std::uint8_t  index{};
};

/**
 * @brief TZIC of an STM32 GTZC under TrustZone, RM0456 5.4.5, 5.5 and 5.7. Every register is
 *        Secure privileged only: a Non-secure access reads zero, drops the write and raises the
 *        TZIC's own event, an unprivileged Secure one reads zero and drops the write. `SRn`
 *        flags are set by illegal_access(), `FCRn` clears them, `IERn` enables them. GTZC_IRQn
 *        is active while an enabled flag of any TZIC of the machine is set, driven through the
 *        GTZC model the description gives the IRQ, GTZC1_MPCBB1 on the U575: a user model in
 *        its place leaves GTZC_IRQn undriven for both TZICs.
 */
class tzic final : public gtzc_block, public illegal_access_controller
{
public:
  tzic(model_context& ctx, peripheral_desc const& desc, gtzc_variant variant)
      : gtzc_block{ctx, desc, "GTZC TZIC"}
  {
    for(auto const& described : desc.registers)
    {
      for(auto const& [prefix, role] : {
              std::pair{std::string_view{"IER"}, tzic_role::enable},
              std::pair{std::string_view{"SR"},  tzic_role::status},
              std::pair{std::string_view{"FCR"}, tzic_role::clear }
      })
      {
        auto const index = numbered(described.name, prefix);
        if(index and *index >= 1)
        {
          claim(described.name);
          words_.push_back(
              {.offset = described.offset & ~3u,
               .role   = role,
               .index  = static_cast<std::uint8_t>(*index - 1)});
          if(role == tzic_role::enable)
          {
            enable_.resize(std::max<std::size_t>(enable_.size(), *index));
            enable_[*index - 1] = described.offset & ~3u;
            valid_.resize(enable_.size());
            valid_[*index - 1] = fields_mask(described);
            for(auto const& bit : described.fields)
            {
              if(not bit.name.ends_with("IE"))
              {
                continue;
              }
              auto const stem = std::string_view{bit.name}.substr(0, bit.name.size() - 2);
              sources_.push_back({
                  .field = bit.name,
                  .at =
                      {.reg  = static_cast<std::uint8_t>(*index - 1),
                           .mask = field_of(described, bit).mask()},
                  .units = units_of_stem(ctx.part, variant, stem)
              });
            }
          }
          if(role == tzic_role::status)
          {
            status_.resize(std::max<std::size_t>(status_.size(), *index));
            status_[*index - 1] = described.offset & ~3u;
          }
        }
      }
    }
  }

  void reset(register_file& regs, reset_kind) override
  {
    for(auto const& name : claims())
    {
      regs.poke(reg(name).offset, describe(name).reset);
    }
  }

  [[nodiscard]] auto source(std::string_view enable_field) const
      -> std::optional<illegal_source> override
  {
    auto const found = std::ranges::find(sources_, enable_field, &tzic_source::field);
    return found != sources_.end() ? std::optional{found->at} : std::nullopt;
  }

  [[nodiscard]] auto source_of(std::size_t unit) const -> std::optional<illegal_source> override
  {
    auto const found = std::ranges::find_if(
        sources_, [&](tzic_source const& s) { return std::ranges::contains(s.units, unit); });
    return found != sources_.end() ? std::optional{found->at} : std::nullopt;
  }

  void illegal_access(illegal_source from) override
  {
    if(from.reg >= status_.size())
    {
      return;
    }
    auto& regs = ctx_.regs;
    regs.poke(status_[from.reg], regs.peek(status_[from.reg]) | (from.mask & valid_at(from.reg)));
    drive();
  }

  [[nodiscard]] auto pending() const -> bool
  {
    for(auto const index :
        std::views::iota(std::size_t{0}, std::min(status_.size(), enable_.size())))
    {
      if((ctx_.regs.peek(status_[index]) & ctx_.regs.peek(enable_[index])) != 0)
      {
        return true;
      }
    }
    return false;
  }

  auto on_read(register_file& regs, std::uint32_t offset, std::uint8_t size, access_ctx ctx)
      -> std::expected<std::uint32_t, bus_error> override
  {
    if(not ctx.secure)
    {
      own_illegal_access(ctx.by);
      return 0;
    }
    auto const* const word = word_at(offset);
    if(not ctx.privileged or word->role == tzic_role::clear)
    {
      return 0;
    }
    return read_lanes(regs.peek(word->offset) & valid_at(word->index), offset, size);
  }

  auto on_write(
      register_file& regs,
      std::uint32_t  offset,
      std::uint8_t   size,
      std::uint32_t,
      std::uint32_t value,
      access_ctx    ctx) -> std::expected<void, bus_error> override
  {
    if(not ctx.secure)
    {
      own_illegal_access(ctx.by);
      return {};
    }
    auto const* const word = word_at(offset);
    if(not ctx.privileged or word->role == tzic_role::status)
    {
      return {};
    }
    auto const merged = merge_write(regs.peek(word->offset), offset, size, value);
    if(word->role == tzic_role::enable)
    {
      regs.poke(word->offset, merged & valid_at(word->index));
    }
    else if(word->index < status_.size())
    {
      auto const at = status_[word->index];
      regs.poke(at, regs.peek(at) & ~(merged & valid_at(word->index)));
    }
    drive();
    return {};
  }

private:
  [[nodiscard]] auto word_at(std::uint32_t offset) const -> tzic_word const*
  {
    return &*std::ranges::find(words_, offset & ~3u, &tzic_word::offset);
  }

  [[nodiscard]] auto valid_at(std::uint8_t index) const -> std::uint32_t
  {
    return index < valid_.size() ? valid_[index] : 0;
  }

  void drive()
  {
    if(not linked_ and not ctx_.peers.all().empty())
    {
      linked_ = true;
      for(auto* const peer : ctx_.peers.all())
      {
        if(auto* const block = dynamic_cast<gtzc_block*>(peer);
           block != nullptr and block->gtzc_line() != nullptr)
        {
          line_ = block->gtzc_line();
        }
        if(auto* const other = dynamic_cast<tzic*>(peer))
        {
          tzics_.push_back(other);
        }
      }
    }
    if(line_ == nullptr)
    {
      return;
    }
    if(std::ranges::any_of(tzics_, &tzic::pending))
    {
      line_->raise();
    }
    else
    {
      line_->lower();
    }
  }

  std::vector<tzic_word>     words_;
  std::vector<tzic_source>   sources_;
  std::vector<std::uint32_t> enable_;
  std::vector<std::uint32_t> status_;
  std::vector<std::uint32_t> valid_;
  bool                       linked_{};
  irq_line*                  line_{};
  std::vector<tzic const*>   tzics_;
};

enum class mpcbb_role : std::uint8_t
{
  control,
  lock,
  security,
  privilege,
};

struct mpcbb_word
{
  std::uint32_t offset{};
  mpcbb_role    role{};
  std::uint32_t index{};
  std::uint32_t writable{};
};

/**
 * @brief MPCBB of an STM32 GTZC under TrustZone, RM0456 5.4.4, 5.4.6, 5.8 and 5.11. The SRAM
 *        it covers comes from mpcbb_geometry(). Reset sets every implemented `SECCFGRx` and
 *        `PRIVCFGRx` to all ones. `CR` and `CFGLOCKRx` are Secure privileged only, `CR.GLOCK` locks
 * `CR` and each `SPLCKy` bit locks super-block y's `SECCFGRy` and `PRIVCFGRy` until the next reset.
 * `SECCFGRx` and `PRIVCFGRx` read for every transaction. A `SECCFGRx` bit changes only with a
 * Secure transaction, privileged when its `PRIVCFGRx` bit is set, a `PRIVCFGRx` bit only with a
 * privileged one, Secure when its `SECCFGRx` bit is set. A Non-secure write to `CR`, `CFGLOCKRx` or
 * `SECCFGRx` and a Non-secure read of `CR` or `CFGLOCKRx` raise the MPCBB's own TZIC event,
 * RM0456 3.5.5. The registers of super-blocks past the SRAM read zero and drop writes. The memory
 * accesses are filtered by the TZSC firewall from these bits. `CR.INVSECSTATE` is storage.
 */
class mpcbb final : public gtzc_block, public sram_block_controller
{
public:
  mpcbb(model_context& ctx, peripheral_desc const& desc)
      : gtzc_block{ctx, desc, "GTZC MPCBB"}, geometry_{mpcbb_geometry(ctx.part, desc.name)}
  {
    for(auto const& described : desc.registers)
    {
      if(auto const index = numbered(described.name, "SECCFGR"))
      {
        security_.resize(std::max<std::size_t>(security_.size(), *index + 1));
        security_[*index] = described.offset & ~3u;
      }
      if(auto const index = numbered(described.name, "PRIVCFGR"))
      {
        privilege_.resize(std::max<std::size_t>(privilege_.size(), *index + 1));
        privilege_[*index] = described.offset & ~3u;
      }
    }
    if(not geometry_)
    {
      throw config_error(
          ctx.part.part, 0, std::format("peripherals.{}", desc.name),
          "an MPCBB with an SRAM in the board data of its family");
    }
    check_geometry();
    implemented_ = geometry_->size / (geometry_->block_size * geometry_->superblock_blocks);
    control_     = field("CR", "GLOCK");
    secure_data_ = field("CR", "SRWILADIS");
    claim("CR");
    words_.push_back(
        {.offset   = reg("CR").offset,
         .role     = mpcbb_role::control,
         .writable = fields_mask(describe("CR"))});
    for(auto const& described : desc.registers)
    {
      std::uint32_t lockable = 0;
      for(auto const& bit : described.fields)
      {
        auto const block = numbered(bit.name, "SPLCK");
        if(not block)
        {
          continue;
        }
        if(*block < implemented_)
        {
          lockable |= field_of(described, bit).mask();
          locks_.resize(std::max<std::size_t>(locks_.size(), *block + 1));
          locks_[*block] = field_of(described, bit);
        }
      }
      if(std::ranges::any_of(
             described.fields, [](field_desc const& f) { return f.name.starts_with("SPLCK"); }))
      {
        claim(described.name);
        words_.push_back(
            {.offset = described.offset & ~3u, .role = mpcbb_role::lock, .writable = lockable});
      }
    }
    for(auto const& [index, at] : std::views::enumerate(security_))
    {
      claim(std::format("SECCFGR{}", index));
      words_.push_back(
          {.offset = at, .role = mpcbb_role::security, .index = static_cast<std::uint32_t>(index)});
    }
    for(auto const& [index, at] : std::views::enumerate(privilege_))
    {
      claim(std::format("PRIVCFGR{}", index));
      words_.push_back(
          {.offset = at,
           .role   = mpcbb_role::privilege,
           .index  = static_cast<std::uint32_t>(index)});
    }
  }

  void reset(register_file& regs, reset_kind) override
  {
    for(auto const& name : claims())
    {
      regs.poke(reg(name).offset, describe(name).reset);
    }
    for(auto const& [index, at] : std::views::enumerate(security_))
    {
      regs.poke(at, implemented(static_cast<std::uint32_t>(index)) ? ~0u : 0u);
    }
    for(auto const& [index, at] : std::views::enumerate(privilege_))
    {
      regs.poke(at, implemented(static_cast<std::uint32_t>(index)) ? ~0u : 0u);
    }
    ctx_.services.memory_policy_changed();
  }

  [[nodiscard]] auto blocks() const -> std::optional<sram_blocks> override
  {
    return geometry_;
  }

  [[nodiscard]] auto block_secure(address at) const -> std::optional<bool> override
  {
    return block_bit(security_, at);
  }

  [[nodiscard]] auto block_privileged(address at) const -> std::optional<bool> override
  {
    return block_bit(privilege_, at);
  }

  [[nodiscard]] auto secure_data_allowed() const -> bool override
  {
    return secure_data_.of(ctx_.regs.peek(secure_data_.offset)) != 0;
  }

  auto on_read(register_file& regs, std::uint32_t offset, std::uint8_t size, access_ctx ctx)
      -> std::expected<std::uint32_t, bus_error> override
  {
    auto const* const word = word_at(offset);
    switch(word->role)
    {
      case mpcbb_role::control:
      case mpcbb_role::lock:
        if(not ctx.secure)
        {
          own_illegal_access(ctx.by);
          return 0;
        }
        if(not ctx.privileged)
        {
          return 0;
        }
        return read_lanes(regs.peek(word->offset) & word->writable, offset, size);
      case mpcbb_role::security:
      case mpcbb_role::privilege:
        if(not implemented(word->index))
        {
          return 0;
        }
        return read_lanes(regs.peek(word->offset), offset, size);
    }
    return 0;
  }

  auto on_write(
      register_file& regs,
      std::uint32_t  offset,
      std::uint8_t   size,
      std::uint32_t,
      std::uint32_t value,
      access_ctx    ctx) -> std::expected<void, bus_error> override
  {
    auto const* const word   = word_at(offset);
    auto const        old    = regs.peek(word->offset);
    auto const        merged = merge_write(old, offset, size, value);
    switch(word->role)
    {
      case mpcbb_role::control:
        if(not ctx.secure)
        {
          own_illegal_access(ctx.by);
        }
        else if(ctx.privileged and control_.of(old) == 0)
        {
          regs.poke(word->offset, merged & word->writable);
          policy_written(
              old & secure_data_.mask(), regs.peek(word->offset) & secure_data_.mask(), false);
        }
        return {};
      case mpcbb_role::lock:
        if(not ctx.secure)
        {
          own_illegal_access(ctx.by);
        }
        else if(ctx.privileged)
        {
          regs.poke(word->offset, old | (merged & word->writable));
        }
        return {};
      case mpcbb_role::security:
        if(not ctx.secure)
        {
          own_illegal_access(ctx.by);
        }
        else if(implemented(word->index) and not locked(word->index))
        {
          auto const allowed = ctx.privileged ? ~0u : ~regs.peek(privilege_[word->index]);
          regs.poke(word->offset, (merged & allowed) | (old & ~allowed));
          policy_written(old, regs.peek(word->offset), true);
        }
        return {};
      case mpcbb_role::privilege:
        if(ctx.privileged and implemented(word->index) and not locked(word->index))
        {
          auto const allowed = ctx.secure ? ~0u : ~regs.peek(security_[word->index]);
          regs.poke(word->offset, (merged & allowed) | (old & ~allowed));
          policy_written(old, regs.peek(word->offset), true);
        }
        return {};
    }
    return {};
  }

private:
  void policy_written(std::uint32_t old, std::uint32_t now, bool fetch)
  {
    if(old == now)
    {
      return;
    }
    ctx_.services.memory_policy_changed();
    if(fetch)
    {
      ctx_.services.end_after_instruction();
    }
  }

  void check_geometry() const
  {
    auto const& g       = *geometry_;
    auto const  span    = std::uint64_t{g.block_size} * g.superblock_blocks;
    auto const  covered = std::ranges::any_of(ctx_.part.regions, [&](region_desc const& r) {
      return r.kind == region_kind::ram
         and r.start <= g.start
         and std::uint64_t{g.start} + g.size <= std::uint64_t{r.start} + r.size;
    });
    if(g.superblock_blocks != 32
       or g.block_size == 0
       or g.size % span != 0
       or g.size / span > security_.size()
       or g.size / span > privilege_.size()
       or not covered)
    {
      throw config_error(
          ctx_.part.part, 0, std::format("peripherals.{}", desc_.name),
          "an MPCBB geometry of 32-block super-blocks, one SECCFGRx and PRIVCFGRx per "
          "super-block, over a RAM region of the part");
    }
  }

  [[nodiscard]] auto implemented(std::uint32_t index) const -> bool
  {
    return index < implemented_;
  }

  [[nodiscard]] auto locked(std::uint32_t index) const -> bool
  {
    return index < locks_.size() and locks_[index].of(ctx_.regs.peek(locks_[index].offset)) != 0;
  }

  [[nodiscard]] auto block_bit(std::vector<std::uint32_t> const& words, address at) const
      -> std::optional<bool>
  {
    if(not geometry_ or at < geometry_->start or at - geometry_->start >= geometry_->size)
    {
      return std::nullopt;
    }
    auto const block = static_cast<std::uint32_t>((at - geometry_->start) / geometry_->block_size);
    auto const index = block / geometry_->superblock_blocks;
    auto const bit   = block % geometry_->superblock_blocks;
    return (ctx_.regs.peek(words[index]) >> bit & 1u) != 0;
  }

  [[nodiscard]] auto word_at(std::uint32_t offset) const -> mpcbb_word const*
  {
    return &*std::ranges::find(words_, offset & ~3u, &mpcbb_word::offset);
  }

  std::optional<sram_blocks> geometry_;
  std::uint32_t              implemented_{};
  field_ref                  control_;
  field_ref                  secure_data_;
  std::vector<std::uint32_t> security_;
  std::vector<std::uint32_t> privilege_;
  std::vector<field_ref>     locks_;
  std::vector<mpcbb_word>    words_;
};
}

auto gtzc_fields(part_description const& part, gtzc_variant variant) -> std::vector<gtzc_field>
{
  std::vector<gtzc_field> out;
  for(auto const& unit : part.peripherals)
  {
    auto const tzsc_block = is_tzsc(unit);
    if(not tzsc_block and not is_tzic(unit))
    {
      continue;
    }
    auto const prefix = tzsc_block ? std::string_view{"SECCFGR"} : std::string_view{"IER"};
    auto const suffix = tzsc_block ? std::string_view{"SEC"} : std::string_view{"IE"};
    for(auto const& described : unit.registers)
    {
      if(not numbered(described.name, prefix))
      {
        continue;
      }
      for(auto const& bit : described.fields)
      {
        if(not bit.name.ends_with(suffix))
        {
          continue;
        }
        auto const stem    = std::string_view{bit.name}.substr(0, bit.name.size() - suffix.size());
        auto const memory  = memory_stem(stem);
        auto       targets = units_of_stem(part, variant, stem);
        out.push_back(
            {.owner  = unit.name,
             .reg    = described.name,
             .field  = bit.name,
             .target = memory              ? gtzc_target::memory
                     : not targets.empty() ? gtzc_target::peripheral
                                           : gtzc_target::unknown,
             .units  = std::move(targets)});
      }
    }
  }
  return out;
}

auto unit_secure(peer_directory const& peers, part_description const& part, std::size_t unit)
    -> bool
{
  for(auto* const peer : peers.all())
  {
    if(auto* const firewall = dynamic_cast<peripheral_security*>(peer))
    {
      if(auto const secure = firewall->peripheral_secure(unit))
      {
        return *secure;
      }
    }
  }
  auto const* const owner = peers.find(part.peripherals[unit].name);
  auto const* const aware = dynamic_cast<trustzone_aware const*>(owner);
  return aware != nullptr and aware->secure_function();
}

void illegal_event::raise(peer_directory const& peers, std::size_t unit, initiator by)
{
  if(by == initiator::debugger)
  {
    return;
  }
  if(not linked_ and not peers.all().empty())
  {
    linked_ = true;
    for(auto* const peer : peers.all())
    {
      if(auto* const tzic = dynamic_cast<illegal_access_controller*>(peer))
      {
        if(auto const source = tzic->source_of(unit))
        {
          tzic_   = tzic;
          source_ = *source;
          break;
        }
      }
    }
  }
  if(tzic_ != nullptr)
  {
    tzic_->illegal_access(source_);
  }
}

auto gtzc_factory(gtzc_variant variant) -> model_factory
{
  return [variant](model_context& ctx, peripheral_desc const& desc) -> std::shared_ptr<model> {
    if(not ctx.trustzone)
    {
      return nullptr;
    }
    if(desc.name.ends_with("_TZSC"))
    {
      return std::make_shared<tzsc>(ctx, desc, variant);
    }
    if(desc.name.ends_with("_TZIC"))
    {
      return std::make_shared<tzic>(ctx, desc, variant);
    }
    if(desc.name.contains("_MPCBB"))
    {
      return std::make_shared<mpcbb>(ctx, desc);
    }
    return nullptr;
  };
}
}
