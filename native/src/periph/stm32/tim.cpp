#include "dma_signal.hpp"
#include "factories.hpp"
#include "model_base.hpp"

#include <popcorn/core/clock.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace popcorn::periph::stm32
{
namespace
{
using wide = unsigned __int128;

constexpr std::uint64_t ps_per_second = 1'000'000'000'000;
constexpr std::uint32_t tick_due      = 0;
constexpr std::size_t   max_channels  = 4;

struct tim_state
{
  std::uint64_t                           anchor_ps{};
  std::uint64_t                           anchor_frac{};
  std::uint64_t                           hz{};
  std::uint64_t                           due_ps{};
  std::uint64_t                           carry{};
  std::uint32_t                           generation{};
  std::uint32_t                           count{};
  std::uint32_t                           repetition{};
  std::uint32_t                           prescaler{};
  std::uint32_t                           reload{};
  std::array<std::uint32_t, max_channels> compare{};
  std::array<std::uint8_t, max_channels>  compare_request{};
  std::uint8_t                            update_request{};
  std::uint8_t                            down{};
  std::uint8_t                            scheduled{};

  friend auto operator==(tim_state const&, tim_state const&) -> bool = default;
};

struct channel
{
  reg_ref                   ccr;
  field_ref                 select;
  std::optional<field_ref>  preload;
  field_ref                 flag;
  std::optional<field_ref>  overcapture;
  field_ref                 interrupt;
  std::optional<field_ref>  dma;
  field_ref                 generate;
  std::optional<dma_signal> signal;
};

struct interrupt_source
{
  field_ref   enable;
  field_ref   flag;
  std::size_t line;
};

auto lower(std::string_view name) -> std::string
{
  std::string out{name};
  std::ranges::transform(
      out, out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

/**
 * @brief TIM1, TIM2 to TIM7 and TIM15 to TIM17 of the STM32U5, RM0456 sections 54 "Advanced-control
 *        timers (TIM1/TIM8)", 55 "General-purpose timers (TIM2/TIM3/TIM4/TIM5)", 56 "General
 *        purpose timers (TIM15/TIM16/TIM17)" and 57 "Basic timers (TIM6/TIM7)": the time-base
 *        unit on the instance's timer clock. The counter is derived from the virtual time of its
 *        last anchor, never ticked: up, down and the three center-aligned modes, the prescaler,
 *        auto-reload and compare preloads loaded at the update event, the repetition counter,
 *        UDIS, URS, one-pulse mode, EGR event generation. Output compare channels 1 to 4 set
 *        their flags when the counter matches, output pins are not driven. The counter width is
 *        the sum of the description's CNT fields. With UDE or CCxDE the `<instance>_upd_dma` and
 *        `<instance>_ccx_dma` requests rise with their event, and any DMA access to the timer
 *        releases them all. Deviations: an ARR written below the count lets the counter run to
 *        its maximum and wrap with no update, a count written above ARR in center-aligned mode
 *        does the same, and UIFREMAP puts UIFCPY in bit 31 only where the description names a
 *        UIFCPY field, not on the 32-bit TIM2 to TIM5. Not modelled, logged once when the counter
 *        starts: slave modes, input capture edges, dithering, CCDS. DMA burst mode is not
 *        modelled, logged once when DCR selects it.
 */
class tim final : public dispatch_model<tim, tim_state>, public clock_listener
{
public:
  tim(model_context& ctx, peripheral_desc const& desc);

  void reset(register_file& regs, reset_kind kind) override;
  void on_event(std::uint32_t kind, std::uint64_t payload) override;
  auto on_read(register_file& regs, std::uint32_t offset, std::uint8_t size, access_ctx ctx)
      -> std::expected<std::uint32_t, bus_error> override;
  auto on_write(
      register_file& regs,
      std::uint32_t  offset,
      std::uint8_t   size,
      std::uint32_t  old,
      std::uint32_t  value,
      access_ctx     ctx) -> std::expected<void, bus_error> override;
  void on_clock_change() override;

  void                           control_written(std::uint32_t old, std::uint32_t now);
  void                           status_written(std::uint32_t old, std::uint32_t now);
  void                           event_written(std::uint32_t old, std::uint32_t now);
  void                           counter_written(std::uint32_t old, std::uint32_t now);
  void                           reload_written(std::uint32_t old, std::uint32_t now);
  void                           burst_written(std::uint32_t old, std::uint32_t now);
  template <std::size_t Ch> void compare_written(std::uint32_t old, std::uint32_t now);
  void                           plain_written(std::uint32_t old, std::uint32_t now);

private:
  [[nodiscard]] auto now_ps() const -> std::uint64_t;
  [[nodiscard]] auto name(std::string_view reg) const -> std::string;
  [[nodiscard]] auto optional_field(std::string_view reg, std::string_view name) const
      -> std::optional<field_ref>;
  [[nodiscard]] auto center() const -> bool;
  [[nodiscard]] auto counting_up() const -> bool;
  [[nodiscard]] auto active() const -> bool;
  [[nodiscard]] auto counting() const -> bool;
  [[nodiscard]] auto flagged_when(bool up) const -> bool;
  [[nodiscard]] auto anchor() const -> wide;
  [[nodiscard]] auto tick_unit() const -> wide;
  [[nodiscard]] auto ticks_until(std::uint64_t ps) const -> std::uint64_t;
  [[nodiscard]] auto ticks_to_wrap() const -> std::uint64_t;
  [[nodiscard]] auto ticks_to_match(std::size_t ch, std::uint64_t wrap) const -> std::uint64_t;
  [[nodiscard]] auto canonical() const -> bool;
  [[nodiscard]] auto pending() const -> bool;
  [[nodiscard]] auto output(std::size_t ch) const -> bool;
  [[nodiscard]] auto compare_value(std::size_t ch) const -> std::uint32_t;
  [[nodiscard]] auto repetition_reload() const -> std::uint32_t;
  [[nodiscard]] auto wants_update() const -> bool;
  [[nodiscard]] auto wants_match(std::size_t ch) const -> bool;

  void start_at(std::uint64_t ps);
  void rebase(std::uint64_t ps, std::uint64_t hz);
  void advance(std::uint64_t ticks);
  void catch_up(std::uint64_t ps);
  void visit(std::uint64_t ticks);
  void wrap();
  void skip_groups(std::uint64_t groups);
  void update_event(bool software);
  void match(std::size_t ch);
  void capture(std::size_t ch);
  void request_update();
  void acknowledge();
  void settle();
  void reschedule();
  void warn_unsupported();

  register_file&                               regs_;
  std::string                                  prefix_;
  std::uint32_t                                mask_{};
  field_ref                                    enable_;
  field_ref                                    no_update_;
  field_ref                                    update_source_;
  field_ref                                    one_pulse_;
  field_ref                                    reload_preload_;
  field_ref                                    remap_;
  std::optional<field_ref>                     direction_;
  std::optional<field_ref>                     center_mode_;
  field_ref                                    update_flag_;
  field_ref                                    update_interrupt_;
  std::optional<field_ref>                     update_dma_;
  field_ref                                    update_generate_;
  reg_ref                                      status_;
  reg_ref                                      events_;
  reg_ref                                      counter_;
  std::optional<field_ref>                     copy_;
  field_ref                                    prescaler_;
  reg_ref                                      reload_;
  std::optional<field_ref>                     repetition_;
  std::optional<dma_signal>                    update_signal_;
  std::vector<channel>                         channels_;
  std::vector<irq_line*>                       lines_;
  std::vector<interrupt_source>                sources_;
  std::vector<std::pair<field_ref, field_ref>> generated_;
  std::vector<field_ref>                       unsupported_;
  core::clock_domain                           domain_;
  std::optional<field_ref>                     burst_length_;
  std::optional<field_ref>                     burst_base_;
  bool                                         unsupported_logged_{};
  bool                                         burst_logged_{};
};

template <std::size_t... Ch>
auto compare_entries(std::string_view prefix, std::index_sequence<Ch...>)
    -> std::array<write_entry<tim>, sizeof...(Ch)>
{
  static constexpr std::array<std::string_view, 4> plain{"CCR1", "CCR2", "CCR3", "CCR4"};
  static constexpr std::array<std::string_view, 4> prefixed{
      "TIM1_CCR1", "TIM1_CCR2", "TIM1_CCR3", "TIM1_CCR4"};
  auto const& names = prefix.empty() ? plain : prefixed;
  return {{{names[Ch], &tim::compare_written<Ch>}...}};
}

auto tim_writes(peripheral_desc const& desc) -> std::vector<write_entry<tim>>
{
  static constexpr std::array<write_entry<tim>, 10> plain{
      {
       {"CR1", &tim::control_written},
       {"DIER", &tim::plain_written},
       {"SR", &tim::status_written},
       {"EGR", &tim::event_written},
       {"CNT", &tim::counter_written},
       {"PSC", &tim::plain_written},
       {"ARR", &tim::reload_written},
       {"RCR", &tim::plain_written},
       {"DCR", &tim::burst_written},
       {"DMAR", &tim::plain_written},
       }
  };
  static constexpr std::array<write_entry<tim>, 10> prefixed{
      {
       {"TIM1_CR1", &tim::control_written},
       {"TIM1_DIER", &tim::plain_written},
       {"TIM1_SR", &tim::status_written},
       {"TIM1_EGR", &tim::event_written},
       {"TIM1_CNT", &tim::counter_written},
       {"TIM1_PSC", &tim::plain_written},
       {"TIM1_ARR", &tim::reload_written},
       {"TIM1_RCR", &tim::plain_written},
       {"TIM1_DCR", &tim::burst_written},
       {"TIM1_DMAR", &tim::plain_written},
       }
  };
  auto const has = [&](std::string_view reg) {
    return std::ranges::contains(desc.registers, reg, &register_desc::name);
  };
  auto const                    prefix = has("TIM1_CR1") ? std::string_view{"TIM1_"} : "";
  std::vector<write_entry<tim>> out;
  for(auto const& entry : prefix.empty() ? plain : prefixed)
  {
    auto const optional =
        entry.reg.ends_with("RCR") or entry.reg.ends_with("DCR") or entry.reg.ends_with("DMAR");
    if(has(entry.reg) or not optional)
    {
      out.push_back(entry);
    }
  }
  for(auto const& entry : compare_entries(prefix, std::make_index_sequence<max_channels>{}))
  {
    if(has(entry.reg))
    {
      out.push_back(entry);
    }
  }
  return out;
}

auto multiline_group(std::string_view base) -> std::string_view
{
  if(base == "U")
  {
    return "UP";
  }
  if(base == "B" or base == "B2" or base == "SB")
  {
    return "BRK";
  }
  if(base.starts_with("CC"))
  {
    return "CC";
  }
  return "TRG_COM";
}

tim::tim(model_context& ctx, peripheral_desc const& desc)
    : dispatch_model{ctx, desc, "TIM", tim_writes(desc)}, regs_{ctx.regs},
      prefix_{has_register("TIM1_CR1") ? "TIM1_" : ""}, enable_{field(name("CR1"), "CEN")},
      no_update_{field(name("CR1"), "UDIS")}, update_source_{field(name("CR1"), "URS")},
      one_pulse_{field(name("CR1"), "OPM")}, reload_preload_{field(name("CR1"), "ARPE")},
      remap_{field(name("CR1"), "UIFREMAP")}, direction_{optional_field(name("CR1"), "DIR")},
      center_mode_{optional_field(name("CR1"), "CMS")}, update_flag_{field(name("SR"), "UIF")},
      update_interrupt_{field(name("DIER"), "UIE")},
      update_dma_{optional_field(name("DIER"), "UDE")}, update_generate_{field(name("EGR"), "UG")},
      status_{reg(name("SR"))}, events_{reg(name("EGR"))}, counter_{reg(name("CNT"))},
      copy_{optional_field(name("CNT"), "UIFCPY")}, prescaler_{field(name("PSC"), "PSC")},
      reload_{reg(name("ARR"))}, repetition_{optional_field(name("RCR"), "REP")},
      domain_{ctx.clock.domain(lower(desc.name))},
      burst_length_{optional_field(name("DCR"), "DBL")},
      burst_base_{optional_field(name("DCR"), "DBA")}
{
  std::uint32_t top = 0;
  for(auto const& bits : describe(name("CNT")).fields)
  {
    if(bits.name != "UIFCPY")
    {
      top = std::max<std::uint32_t>(top, bits.lsb + bits.width);
    }
  }
  if(top == 0)
  {
    missing(std::format("{}.CNT", name("CNT")));
  }
  mask_               = top >= 32 ? ~std::uint32_t{} : (std::uint32_t{1} << top) - 1u;
  auto const instance = lower(desc.name);
  if(update_dma_)
  {
    update_signal_.emplace(ctx.peers, instance + "_upd_dma");
  }
  for(std::size_t ch = 0; ch < max_channels; ++ch)
  {
    auto const n   = ch + 1;
    auto const ccr = name(std::format("CCR{}", n));
    if(not has_register(ccr))
    {
      break;
    }
    auto const               mode = n <= 2 ? 1 : 2;
    auto const               pe   = std::format("OC{}PE", n);
    std::optional<field_ref> preload;
    std::string              view;
    for(auto const& candidate :
        {std::format("{}CCMR{}_Output", prefix_, mode),
         std::format("{}CCMR{}_ALTERNATE1", prefix_, mode)})
    {
      if(has_register(candidate)
         and std::ranges::contains(describe(candidate).fields, pe, &field_desc::name))
      {
        view    = candidate;
        preload = field(candidate, pe);
        break;
      }
    }
    if(view.empty())
    {
      missing(std::format("{}CCMR{}.{}", prefix_, mode, pe));
    }
    auto& c       = channels_.emplace_back();
    c.ccr         = reg(ccr);
    c.select      = field(view, std::format("CC{}S", n));
    c.preload     = preload;
    c.flag        = field(name("SR"), std::format("CC{}IF", n));
    c.overcapture = optional_field(name("SR"), std::format("CC{}OF", n));
    c.interrupt   = field(name("DIER"), std::format("CC{}IE", n));
    c.dma         = optional_field(name("DIER"), std::format("CC{}DE", n));
    c.generate    = field(name("EGR"), std::format("CC{}G", n));
    if(c.dma)
    {
      c.signal.emplace(ctx.peers, std::format("{}_cc{}_dma", instance, n));
    }
  }
  auto const multiline =
      std::ranges::contains(ctx.part.irqs, std::format("{}_UP_IRQn", desc.name), &irq_desc::name);
  std::vector<std::string> groups;
  auto const               line_of = [&](std::string_view base) -> std::size_t {
    auto const irq_name = multiline ? std::format("{}_{}_IRQn", desc.name, multiline_group(base))
                                    : std::format("{}_IRQn", desc.name);
    auto const at       = std::ranges::find(groups, irq_name);
    if(at != groups.end())
    {
      return static_cast<std::size_t>(at - groups.begin());
    }
    lines_.push_back(&irq(irq_name));
    groups.push_back(irq_name);
    return lines_.size() - 1;
  };
  auto const& flags = describe(name("SR")).fields;
  for(auto const& enable : describe(name("DIER")).fields)
  {
    std::string_view const enable_name{enable.name};
    if(not enable_name.ends_with("IE"))
    {
      continue;
    }
    auto const base = enable_name.substr(0, enable_name.size() - 2);
    for(auto const& flag_name :
        {std::format("{}IF", base), std::format("{}F", base),
         base == "B" ? std::string{"B2IF"} : std::string{},
         base == "B" ? std::string{"SBIF"} : std::string{}})
    {
      if(not flag_name.empty() and std::ranges::contains(flags, flag_name, &field_desc::name))
      {
        sources_.push_back(
            {.enable = field(name("DIER"), enable_name),
             .flag   = field(name("SR"), flag_name),
             .line   = line_of(base)});
      }
    }
  }
  for(auto const& generate : describe(name("EGR")).fields)
  {
    std::string_view const generate_name{generate.name};
    if(generate_name == "UG" or generate_name.starts_with("CC") or not generate_name.ends_with("G"))
    {
      continue;
    }
    auto const flag_name = std::format("{}IF", generate_name.substr(0, generate_name.size() - 1));
    if(std::ranges::contains(flags, flag_name, &field_desc::name))
    {
      generated_.emplace_back(field(name("EGR"), generate_name), field(name("SR"), flag_name));
    }
  }
  for(auto const& [reg_name, bits] : std::array<std::pair<std::string_view, std::string_view>, 4>{
          {
           {"SMCR", "SMS"},
           {"SMCR", "SMS_bit3"},
           {"CR1", "DITHEN"},
           {"CR2", "CCDS"},
           }
  })
  {
    if(auto const found = optional_field(name(reg_name), bits))
    {
      unsupported_.push_back(*found);
    }
  }
}

auto tim::name(std::string_view reg) const -> std::string
{
  return prefix_ + std::string{reg};
}

auto tim::optional_field(std::string_view reg, std::string_view bits) const
    -> std::optional<field_ref>
{
  if(not has_register(reg)
     or not std::ranges::contains(describe(reg).fields, bits, &field_desc::name))
  {
    return std::nullopt;
  }
  return field(reg, bits);
}

auto tim::now_ps() const -> std::uint64_t
{
  return ctx_.clock.time().ps;
}

auto tim::center() const -> bool
{
  return center_mode_ and get(regs_, *center_mode_) != 0;
}

auto tim::counting_up() const -> bool
{
  if(center())
  {
    return state_.down == 0;
  }
  return not direction_ or get(regs_, *direction_) == 0;
}

auto tim::active() const -> bool
{
  return get(regs_, enable_) != 0 and state_.reload != 0;
}

auto tim::counting() const -> bool
{
  return active() and state_.hz != 0;
}

auto tim::flagged_when(bool up) const -> bool
{
  if(not center())
  {
    return true;
  }
  auto const mode = get(regs_, *center_mode_);
  return mode == 3 or (mode == 2) == up;
}

auto tim::anchor() const -> wide
{
  return wide{state_.anchor_ps} * state_.hz + state_.anchor_frac;
}

auto tim::tick_unit() const -> wide
{
  return (wide{state_.prescaler} + 1) * ps_per_second;
}

auto tim::ticks_until(std::uint64_t ps) const -> std::uint64_t
{
  auto const at    = wide{ps} * state_.hz;
  auto const start = anchor();
  return at > start ? static_cast<std::uint64_t>((at - start) / tick_unit()) : 0;
}

auto tim::ticks_to_wrap() const -> std::uint64_t
{
  std::uint64_t const count  = state_.count;
  std::uint64_t const reload = state_.reload;
  std::uint64_t const top    = mask_;
  if(center())
  {
    if(state_.down != 0)
    {
      return std::max<std::uint64_t>(count, 1);
    }
    return count < reload ? reload - count : top - count + 1;
  }
  if(not counting_up())
  {
    return count + 1;
  }
  return count <= reload ? reload - count + 1 : top - count + 1;
}

auto tim::ticks_to_match(std::size_t ch, std::uint64_t wrap) const -> std::uint64_t
{
  auto const up = counting_up();
  if(not flagged_when(up))
  {
    return wrap;
  }
  std::uint64_t const value = compare_value(ch);
  std::uint64_t const count = state_.count;
  if(up and value > count and value - count < wrap)
  {
    return value - count;
  }
  if(not up and value < count and count - value < wrap)
  {
    return count - value;
  }
  return wrap;
}

auto tim::canonical() const -> bool
{
  if(center())
  {
    return state_.count == 0;
  }
  return state_.count == (counting_up() ? 0 : state_.reload);
}

auto tim::pending() const -> bool
{
  if(state_.prescaler
     != get(regs_, prescaler_)
     or state_.reload
     != (regs_.peek(reload_.offset) & mask_))
  {
    return true;
  }
  for(std::size_t ch = 0; ch < channels_.size(); ++ch)
  {
    if(state_.compare[ch] != (regs_.peek(channels_[ch].ccr.offset) & mask_))
    {
      return true;
    }
  }
  return false;
}

auto tim::output(std::size_t ch) const -> bool
{
  return get(regs_, channels_[ch].select) == 0;
}

auto tim::compare_value(std::size_t ch) const -> std::uint32_t
{
  auto const& c = channels_[ch];
  if(c.preload and get(regs_, *c.preload) != 0)
  {
    return state_.compare[ch];
  }
  return regs_.peek(c.ccr.offset) & mask_;
}

auto tim::repetition_reload() const -> std::uint32_t
{
  return repetition_ ? get(regs_, *repetition_) : 0;
}

auto tim::wants_update() const -> bool
{
  if(get(regs_, no_update_) != 0)
  {
    return false;
  }
  auto const dma = update_dma_ and get(regs_, *update_dma_) != 0 and state_.update_request == 0;
  return get(regs_, update_flag_) == 0 or dma or get(regs_, one_pulse_) != 0;
}

auto tim::wants_match(std::size_t ch) const -> bool
{
  auto const& c = channels_[ch];
  if(not output(ch) or compare_value(ch) > state_.reload)
  {
    return false;
  }
  auto const dma = c.dma and get(regs_, *c.dma) != 0 and state_.compare_request[ch] == 0;
  return get(regs_, c.flag) == 0 or dma;
}

void tim::start_at(std::uint64_t ps)
{
  state_.anchor_ps   = ps;
  state_.anchor_frac = 0;
  state_.carry       = 0;
  state_.hz          = ctx_.clock.hz(domain_);
}

void tim::rebase(std::uint64_t ps, std::uint64_t hz)
{
  auto cycles = wide{state_.carry};
  if(state_.hz != 0)
  {
    auto const at = wide{ps} * state_.hz;
    cycles        = at > anchor() ? (at - anchor()) / ps_per_second : 0;
  }
  state_.hz = hz;
  if(hz == 0)
  {
    state_.carry = static_cast<std::uint64_t>(cycles);
    return;
  }
  state_.carry       = 0;
  auto const at      = wide{ps} * hz;
  auto const back    = cycles * ps_per_second;
  auto const start   = at > back ? at - back : 0;
  state_.anchor_ps   = static_cast<std::uint64_t>(start / hz);
  state_.anchor_frac = static_cast<std::uint64_t>(start % hz);
}

void tim::advance(std::uint64_t ticks)
{
  auto const at      = anchor() + wide{ticks} * tick_unit();
  state_.anchor_ps   = static_cast<std::uint64_t>(at / state_.hz);
  state_.anchor_frac = static_cast<std::uint64_t>(at % state_.hz);
}

void tim::visit(std::uint64_t ticks)
{
  if(ticks == 0)
  {
    return;
  }
  auto const          up    = counting_up();
  std::uint64_t const count = state_.count;
  auto const          low   = up ? count + 1 : count - ticks;
  auto const          high  = up ? count + ticks : count - 1;
  if(flagged_when(up))
  {
    for(std::size_t ch = 0; ch < channels_.size(); ++ch)
    {
      std::uint64_t const value = compare_value(ch);
      if(output(ch) and value >= low and value <= high)
      {
        match(ch);
      }
    }
  }
  state_.count = static_cast<std::uint32_t>(up ? count + ticks : count - ticks);
}

void tim::wrap()
{
  auto const up    = counting_up();
  bool       event = true;
  auto       next  = std::uint32_t{};
  if(center())
  {
    if(state_.down != 0)
    {
      state_.down = 0;
    }
    else if(state_.count < state_.reload)
    {
      next        = state_.reload;
      state_.down = 1;
    }
    else
    {
      event = false;
    }
  }
  else if(up and state_.count > state_.reload)
  {
    event = false;
  }
  if(event)
  {
    if(state_.repetition == 0)
    {
      state_.repetition = repetition_reload();
      if(get(regs_, no_update_) == 0)
      {
        update_event(false);
      }
    }
    else
    {
      --state_.repetition;
    }
  }
  if(not center() and not up)
  {
    next = state_.reload;
  }
  state_.count = next;
  if(flagged_when(up))
  {
    for(std::size_t ch = 0; ch < channels_.size(); ++ch)
    {
      if(output(ch) and compare_value(ch) == next)
      {
        match(ch);
      }
    }
  }
}

void tim::skip_groups(std::uint64_t groups)
{
  auto const          per_group = center() ? 2u : 1u;
  auto const          wraps     = groups * per_group;
  std::uint64_t const left      = state_.repetition;
  std::uint64_t const period    = std::uint64_t{repetition_reload()} + 1;
  auto const          updates   = wraps > left;
  state_.repetition =
      static_cast<std::uint32_t>(updates ? period - 1 - (wraps - left - 1) % period : left - wraps);
  advance(
      groups * (center() ? 2 * std::uint64_t{state_.reload} : state_.reload + std::uint64_t{1}));
  if(updates and get(regs_, no_update_) == 0)
  {
    update_event(false);
  }
  auto const mode = center() ? get(regs_, *center_mode_) : 0;
  for(std::size_t ch = 0; ch < channels_.size(); ++ch)
  {
    auto const value   = compare_value(ch);
    auto const visited = value
                      <= state_.reload
                     and not(mode == 1 and value == state_.reload)
                     and not(mode == 2 and value == 0);
    if(output(ch) and visited)
    {
      match(ch);
    }
  }
}

void tim::catch_up(std::uint64_t ps)
{
  while(counting())
  {
    auto const ticks = ticks_until(ps);
    if(ticks == 0)
    {
      return;
    }
    auto const group =
        center() ? 2 * std::uint64_t{state_.reload} : state_.reload + std::uint64_t{1};
    if(canonical() and ticks >= group)
    {
      auto groups = ticks / group;
      if(get(regs_, no_update_) == 0 and (pending() or get(regs_, one_pulse_) != 0))
      {
        groups = std::min<std::uint64_t>(groups, state_.repetition / (center() ? 2 : 1));
      }
      if(groups > 0)
      {
        skip_groups(groups);
        continue;
      }
    }
    auto const to_wrap = ticks_to_wrap();
    if(ticks < to_wrap)
    {
      visit(ticks);
      advance(ticks);
      return;
    }
    visit(to_wrap - 1);
    advance(to_wrap);
    wrap();
  }
}

void tim::update_event(bool software)
{
  state_.prescaler = get(regs_, prescaler_);
  state_.reload    = regs_.peek(reload_.offset) & mask_;
  for(std::size_t ch = 0; ch < channels_.size(); ++ch)
  {
    state_.compare[ch] = regs_.peek(channels_[ch].ccr.offset) & mask_;
  }
  if(not software or get(regs_, update_source_) == 0)
  {
    update(regs_, update_flag_, 1);
    if(update_dma_ and get(regs_, *update_dma_) != 0)
    {
      request_update();
    }
  }
  if(get(regs_, one_pulse_) != 0)
  {
    update(regs_, enable_, 0);
  }
}

void tim::match(std::size_t ch)
{
  auto& c = channels_[ch];
  update(regs_, c.flag, 1);
  if(c.dma and get(regs_, *c.dma) != 0 and state_.compare_request[ch] == 0)
  {
    state_.compare_request[ch] = 1;
    c.signal->set(true);
  }
}

void tim::capture(std::size_t ch)
{
  auto const& c = channels_[ch];
  if(c.overcapture and get(regs_, c.flag) != 0)
  {
    update(regs_, *c.overcapture, 1);
  }
  regs_.poke(c.ccr.offset, (regs_.peek(c.ccr.offset) & ~mask_) | (state_.count & mask_));
  match(ch);
}

void tim::request_update()
{
  if(state_.update_request == 0)
  {
    state_.update_request = 1;
    update_signal_->set(true);
  }
}

void tim::acknowledge()
{
  if(state_.update_request != 0)
  {
    state_.update_request = 0;
    update_signal_->set(false);
  }
  for(std::size_t ch = 0; ch < channels_.size(); ++ch)
  {
    if(state_.compare_request[ch] != 0)
    {
      state_.compare_request[ch] = 0;
      channels_[ch].signal->set(false);
    }
  }
}

void tim::settle()
{
  auto counter = (regs_.peek(counter_.offset) & ~mask_) | state_.count;
  if(copy_)
  {
    counter = copy_->with(counter, get(regs_, remap_) != 0 ? get(regs_, update_flag_) : 0);
  }
  regs_.poke(counter_.offset, counter);
  if(center() and direction_)
  {
    update(regs_, *direction_, state_.down);
  }
  std::array<bool, 4> levels{};
  for(auto const& source : sources_)
  {
    if(get(regs_, source.flag) != 0 and get(regs_, source.enable) != 0)
    {
      levels[source.line] = true;
    }
  }
  for(std::size_t line = 0; line < lines_.size(); ++line)
  {
    if(levels[line])
    {
      lines_[line]->raise();
    }
    else
    {
      lines_[line]->lower();
    }
  }
  reschedule();
}

void tim::reschedule()
{
  if(not counting())
  {
    state_.scheduled = 0;
    return;
  }
  auto const                   to_wrap = ticks_to_wrap();
  std::optional<std::uint64_t> ticks;
  if(wants_update())
  {
    auto const period = center() ? std::uint64_t{state_.reload} : state_.reload + std::uint64_t{1};
    auto const counts = not counting_up()
                     or state_.count
                      < state_.reload
                     or (not center() and state_.count == state_.reload);
    ticks             = to_wrap + (counts ? state_.repetition * period : 0);
  }
  for(std::size_t ch = 0; ch < channels_.size(); ++ch)
  {
    if(wants_match(ch))
    {
      ticks = std::min(ticks.value_or(to_wrap), ticks_to_match(ch, to_wrap));
    }
  }
  if(not ticks)
  {
    state_.scheduled = 0;
    return;
  }
  auto const at  = anchor() + wide{*ticks} * tick_unit();
  auto const due = std::max(static_cast<std::uint64_t>((at + state_.hz - 1) / state_.hz), now_ps());
  if(state_.scheduled != 0 and state_.due_ps == due)
  {
    return;
  }
  state_.scheduled = 1;
  state_.due_ps    = due;
  ctx_.events.schedule(ctx_.clock.to_icount(vtime{due}), tick_due, ++state_.generation);
}

void tim::warn_unsupported()
{
  if(unsupported_logged_)
  {
    return;
  }
  auto used =
      std::ranges::any_of(unsupported_, [&](field_ref bits) { return get(regs_, bits) != 0; });
  for(std::size_t ch = 0; ch < channels_.size(); ++ch)
  {
    used = used or not output(ch);
  }
  if(used)
  {
    unsupported_logged_ = true;
    warn(
        name("CR1"),
        "the counter started with a slave mode, an input capture channel, dithering or CCDS "
        "configured: not modelled, the time base counts on the internal clock");
  }
}

void tim::reset(register_file& regs, reset_kind)
{
  for(auto const& claimed : claims())
  {
    regs.poke(reg(claimed).offset, describe(claimed).reset);
  }
  auto const generation = state_.generation + 1;
  state_                = {};
  state_.generation     = generation;
  state_.reload         = regs.peek(reload_.offset) & mask_;
  for(std::size_t ch = 0; ch < channels_.size(); ++ch)
  {
    state_.compare[ch] = regs.peek(channels_[ch].ccr.offset) & mask_;
    if(channels_[ch].signal)
    {
      channels_[ch].signal->resolve();
    }
  }
  if(update_signal_)
  {
    update_signal_->resolve();
  }
  for(auto* const line : lines_)
  {
    line->lower();
  }
}

auto tim::on_read(register_file& regs, std::uint32_t offset, std::uint8_t size, access_ctx access)
    -> std::expected<std::uint32_t, bus_error>
{
  catch_up(now_ps());
  if(access.by == initiator::dma)
  {
    acknowledge();
  }
  settle();
  return dispatch_model::on_read(regs, offset, size, access);
}

auto tim::on_write(
    register_file& regs,
    std::uint32_t  offset,
    std::uint8_t   size,
    std::uint32_t  old,
    std::uint32_t  value,
    access_ctx     access) -> std::expected<void, bus_error>
{
  auto const ps = now_ps();
  catch_up(ps);
  if(access.by == initiator::dma)
  {
    acknowledge();
  }
  auto const was    = active();
  auto const result = dispatch_model::on_write(regs, offset, size, old, value, access);
  if(not was and active())
  {
    start_at(ps);
  }
  settle();
  return result;
}

void tim::on_event(std::uint32_t kind, std::uint64_t payload)
{
  if(kind != tick_due or payload != state_.generation or state_.scheduled == 0)
  {
    return;
  }
  state_.scheduled = 0;
  catch_up(now_ps());
  settle();
}

void tim::on_clock_change()
{
  auto const ps = now_ps();
  catch_up(ps);
  if(auto const hz = ctx_.clock.hz(domain_); hz != state_.hz)
  {
    rebase(ps, hz);
  }
  state_.scheduled = 0;
  settle();
}

void tim::control_written(std::uint32_t old, std::uint32_t now)
{
  if(center_mode_ and center_mode_->of(old) == 0 and center_mode_->of(now) != 0)
  {
    state_.down = 0;
  }
  if(reload_preload_.of(now) == 0)
  {
    state_.reload = regs_.peek(reload_.offset) & mask_;
  }
  if(enable_.of(old) == 0 and enable_.of(now) != 0)
  {
    warn_unsupported();
  }
}

void tim::status_written(std::uint32_t old, std::uint32_t now)
{
  regs_.poke(status_.offset, old & now);
}

void tim::event_written(std::uint32_t, std::uint32_t now)
{
  regs_.poke(events_.offset, 0);
  for(auto const& [generate, flag] : generated_)
  {
    if(generate.of(now) != 0)
    {
      update(regs_, flag, 1);
    }
  }
  for(std::size_t ch = 0; ch < channels_.size(); ++ch)
  {
    if(channels_[ch].generate.of(now) == 0)
    {
      continue;
    }
    if(output(ch))
    {
      match(ch);
    }
    else
    {
      capture(ch);
    }
  }
  if(update_generate_.of(now) == 0)
  {
    return;
  }
  if(get(regs_, no_update_) == 0)
  {
    update_event(true);
  }
  state_.repetition = repetition_reload();
  state_.down       = 0;
  state_.count      = counting_up() or center() ? 0 : state_.reload;
  start_at(now_ps());
}

void tim::counter_written(std::uint32_t, std::uint32_t now)
{
  state_.count = now & mask_;
  if(state_.count == 0)
  {
    state_.down = 0;
  }
  else if(center() and state_.count == state_.reload)
  {
    state_.down = 1;
  }
}

void tim::reload_written(std::uint32_t, std::uint32_t)
{
  if(get(regs_, reload_preload_) == 0)
  {
    state_.reload = regs_.peek(reload_.offset) & mask_;
  }
}

template <std::size_t Ch> void tim::compare_written(std::uint32_t, std::uint32_t)
{
  auto const& c = channels_[Ch];
  if(not c.preload or get(regs_, *c.preload) == 0)
  {
    state_.compare[Ch] = regs_.peek(c.ccr.offset) & mask_;
  }
}

void tim::burst_written(std::uint32_t, std::uint32_t now)
{
  auto const used = (burst_length_ and burst_length_->of(now) != 0)
                 or (burst_base_ and burst_base_->of(now) != 0);
  if(used and not std::exchange(burst_logged_, true))
  {
    warn(name("DCR"), "DMA burst mode is not modelled: DMAR is plain storage");
  }
}

void tim::plain_written(std::uint32_t, std::uint32_t)
{}
}

auto tim_factory() -> model_factory
{
  return [](model_context& ctx, peripheral_desc const& desc) -> std::shared_ptr<model> {
    return std::make_shared<tim>(ctx, desc);
  };
}
}
