#include "dma_signal.hpp"
#include "factories.hpp"
#include "model_base.hpp"
#include "trigger_signal.hpp"

#include <popcorn/core/clock.hpp>

#include <reflex/enum.hpp>

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

constexpr std::uint64_t ps_per_second        = 1'000'000'000'000;
constexpr std::uint32_t tick_due             = 0;
constexpr std::size_t   max_channels         = 4;
constexpr std::uint32_t master_reset         = 0;
constexpr std::uint32_t master_enable        = 1;
constexpr std::uint32_t master_update        = 2;
constexpr std::uint32_t master_compare_pulse = 3;

enum class slave_mode : std::uint8_t
{
  disabled      = 0b0000,
  encoder1      = 0b0001,
  encoder2      = 0b0010,
  encoder3      = 0b0011,
  reset         = 0b0100,
  gated         = 0b0101,
  trigger       = 0b0110,
  external1     = 0b0111,
  reset_trigger = 0b1000,
  gated_reset   = 0b1001,
};

constexpr std::array<std::pair<std::uint32_t, std::string_view>, 7> internal_triggers{
    {
     {0, "tim1"},
     {1, "tim2"},
     {2, "tim3"},
     {3, "tim4"},
     {8, "tim5"},
     {9, "tim8"},
     {10, "tim15"},
     }
};

[[nodiscard]] constexpr auto modelled(std::uint32_t mode) -> bool
{
  using enum slave_mode;
  return mode == std::to_underlying(reset)
      or mode == std::to_underlying(gated)
      or mode == std::to_underlying(trigger)
      or mode == std::to_underlying(external1)
      or mode == std::to_underlying(reset_trigger)
      or mode == std::to_underlying(gated_reset);
}

[[nodiscard]] constexpr auto gating(std::uint32_t mode) -> bool
{
  return mode == std::to_underlying(slave_mode::gated)
      or mode == std::to_underlying(slave_mode::gated_reset);
}

[[nodiscard]] constexpr auto encoding(std::uint32_t mode) -> bool
{
  return (mode >= std::to_underlying(slave_mode::encoder1)
          and mode <= std::to_underlying(slave_mode::encoder3))
      or mode >= 0b1010;
}

class reacting_scope
{
public:
  explicit reacting_scope(bool& flag) : flag_{flag}, was_{std::exchange(flag, true)}
  {}
  reacting_scope(reacting_scope const&)                    = delete;
  auto operator=(reacting_scope const&) -> reacting_scope& = delete;
  reacting_scope(reacting_scope&&)                         = delete;
  auto operator=(reacting_scope&&) -> reacting_scope&      = delete;
  ~reacting_scope()
  {
    flag_ = was_;
  }

private:
  bool& flag_;
  bool  was_;
};

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
  std::uint32_t                           edges{};
  std::array<std::uint32_t, max_channels> compare{};
  std::array<std::uint8_t, max_channels>  compare_request{};
  std::uint8_t                            update_request{};
  std::uint8_t                            down{};
  std::uint8_t                            scheduled{};
  std::uint8_t                            gate{};

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
 *        releases them all. The trigger output `<instance>_trgo` gives its edges to every
 *        listening trigger_sink as CR2.MMS selects. MMS 001 is the counter enable level, CEN and,
 *        in gated modes, the gate: a rising edge when it sets, a falling edge when it clears, by a
 *        write, at the one-pulse update or at a gate edge. MMS 000,
 *        010 and 011 are positive pulses one timer kernel clock wide, rising at the event and
 *        falling one clock later: 000 at each UG write, 010 at each update event, a UG one
 *        included whatever URS, 011 at each CC1IF set by a match or a capture. Each edge carries
 *        the time of its event. While a sink listens under MMS 010 or 011,
 *        every update or CC1 match is scheduled at its own time instead of being skipped. MMS
 *        100 and above, MMS2 and TRGO2 log a warning once and pulse nothing. The slave mode
 *        controller, RM0456 55.4 "Slave mode" and TIMx_SMCR, listens as a trigger_sink to the
 *        `<instance>_trgo` that TS selects among the internal triggers ITR0 TIM1, ITR1 TIM2, ITR2
 *        TIM3, ITR3 TIM4, ITR4 TIM5, ITR5 TIM8 and ITR6 TIM15. Reset mode reinitialises the
 *        counter and its prescaler at a rising edge, with the update event URS allows. Trigger
 *        mode sets CEN at a rising edge and counts from the edge's time. Reset + trigger does
 *        both. External clock mode 1 counts one tick per PSC + 1 rising edges, at the edge's time,
 *        and never on the timer clock. Gated mode counts on the timer clock while the master's
 *        level is high, a level only an MMS 001 master gives: under a pulsing master the counter
 *        holds and one warning is logged at its first rising edge. The gate is sampled again when
 *        the timer clock comes back. Gated + reset also reinitialises the counter when the
 *        gate closes. Every slave edge sets TIF, gate openings and closings included. ITR7,
 *        ITR8, ITR11, a timer's own ITR, the reserved codes and the timer input pins reach
 *        nothing: the slave holds and one warning is logged. Deviations: an ARR written below the
 *        count lets the counter run to its maximum and wrap with no update, a count written above
 *        ARR in center-aligned mode does the same, UIFREMAP puts UIFCPY in bit 31 only where the
 *        description names a UIFCPY field, not on the 32-bit TIM2 to TIM5, and a trigger cycle
 *        between timers drops the edge that would re-enter a timer still reacting to its own
 *        event, logged once per timer, where hardware would oscillate with a few clocks of
 *        resynchronisation delay per hop. Not modelled, logged once when the counter starts:
 *        encoder modes, which count on the timer clock, SMSPE, input capture edges, dithering,
 *        CCDS. TDE logs once and raises no trigger DMA request. DMA burst mode is not modelled,
 *        logged once when DCR selects it.
 */
class tim final : public dispatch_model<tim, tim_state>,
                  public clock_listener,
                  public trigger_source,
                  public trigger_sink
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
  void               on_clock_change() override;
  void               interest_changed() override;
  [[nodiscard]] auto produces(std::string_view signal) const -> bool override;
  [[nodiscard]] auto level(std::string_view signal) const -> std::optional<bool> override;
  [[nodiscard]] auto listens(std::string_view signal) const -> bool override;
  void triggered(std::string_view signal, trigger_edge edge, std::uint64_t at_ps) override;

  void                           control_written(std::uint32_t old, std::uint32_t now);
  void                           master_written(std::uint32_t old, std::uint32_t now);
  void                           slave_written(std::uint32_t old, std::uint32_t now);
  void                           interrupts_written(std::uint32_t old, std::uint32_t now);
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
  [[nodiscard]] auto running() const -> bool;
  [[nodiscard]] auto internally_clocked() const -> bool;
  [[nodiscard]] auto gate_open() const -> bool;
  [[nodiscard]] auto counter_enabled() const -> bool;
  [[nodiscard]] auto input_level(std::uint32_t smcr) const -> std::optional<bool>;
  [[nodiscard]] auto slave_of(std::uint32_t smcr) const -> std::uint32_t;
  [[nodiscard]] auto slave() const -> std::uint32_t;
  [[nodiscard]] auto trigger_select_of(std::uint32_t smcr) const -> std::uint32_t;
  [[nodiscard]] auto input_of(std::uint32_t smcr) const -> std::optional<std::string_view>;
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
  [[nodiscard]] auto wants_update() -> bool;
  [[nodiscard]] auto wants_match(std::size_t ch) -> bool;
  [[nodiscard]] auto master_mode() const -> std::uint32_t;
  [[nodiscard]] auto master_mode_of(std::uint32_t cr2) const -> std::uint32_t;
  [[nodiscard]] auto trgo_wanted(std::uint32_t mode) -> bool;
  [[nodiscard]] auto at_ps(wide at) const -> std::uint64_t;

  void start_at(std::uint64_t ps);
  void rebase(std::uint64_t ps, std::uint64_t hz);
  void advance(std::uint64_t ticks);
  void catch_up(std::uint64_t ps);
  void visit(std::uint64_t ticks, std::optional<std::uint64_t> at = std::nullopt);
  void wrap(std::uint64_t at);
  void external_tick(std::uint64_t at);
  void reinitialise(std::uint64_t at);
  void begin(std::uint64_t at);
  void gate(std::string_view signal, trigger_edge edge, bool closing_resets, std::uint64_t at);
  void enable_changed(bool was, std::uint64_t at);
  void slave_settled(std::optional<std::string_view> before, std::uint32_t smcr);
  void skip_groups(std::uint64_t groups);
  void update_event(bool software, std::uint64_t at);
  void pulse_trgo(std::uint64_t at);
  void enable_edge(trigger_edge edge, std::uint64_t at);
  void match(std::size_t ch, std::uint64_t at);
  void capture(std::size_t ch, std::uint64_t at);
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
  std::optional<field_ref>                     master_;
  std::optional<field_ref>                     master_high_;
  std::optional<field_ref>                     master2_;
  std::optional<trigger_output>                trgo_;
  std::string                                  instance_;
  std::optional<field_ref>                     slave_;
  std::optional<field_ref>                     slave_high_;
  std::optional<field_ref>                     select_;
  std::optional<field_ref>                     select_high_;
  std::optional<field_ref>                     slave_preload_;
  std::optional<field_ref>                     trigger_flag_;
  std::optional<field_ref>                     trigger_dma_;
  bool                                         reacting_{};
  bool                                         cycle_logged_{};
  bool                                         unrouted_logged_{};
  bool                                         gate_logged_{};
  bool                                         trigger_dma_logged_{};
  bool                                         unsupported_logged_{};
  bool                                         burst_logged_{};
  bool                                         master_logged_{};
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
  static constexpr std::array<write_entry<tim>, 12> plain{
      {
       {"CR1", &tim::control_written},
       {"CR2", &tim::master_written},
       {"SMCR", &tim::slave_written},
       {"DIER", &tim::interrupts_written},
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
  static constexpr std::array<write_entry<tim>, 12> prefixed{
      {
       {"TIM1_CR1", &tim::control_written},
       {"TIM1_CR2", &tim::master_written},
       {"TIM1_SMCR", &tim::slave_written},
       {"TIM1_DIER", &tim::interrupts_written},
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
    auto const optional = entry.reg.ends_with("CR2")
                       or entry.reg.ends_with("SMCR")
                       or entry.reg.ends_with("RCR")
                       or entry.reg.ends_with("DCR")
                       or entry.reg.ends_with("DMAR");
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
      burst_base_{optional_field(name("DCR"), "DBA")}, master_{optional_field(name("CR2"), "MMS")},
      master_high_{optional_field(name("CR2"), "MMS_3")},
      master2_{optional_field(name("CR2"), "MMS2")}, instance_{lower(desc.name)},
      slave_{optional_field(name("SMCR"), "SMS")}, select_{optional_field(name("SMCR"), "TS")},
      slave_preload_{optional_field(name("SMCR"), "SMSPE")},
      trigger_dma_{optional_field(name("DIER"), "TDE")}
{
  if(not master_high_)
  {
    master_high_ = optional_field(name("CR2"), "MMS_1");
  }
  for(auto const bits : {"SMS_1", "SMS_bit3", "SMS_3"})
  {
    slave_high_ = slave_high_ ? slave_high_ : optional_field(name("SMCR"), bits);
  }
  for(auto const bits : {"TS_1", "TS_4_3"})
  {
    select_high_ = select_high_ ? select_high_ : optional_field(name("SMCR"), bits);
  }
  if(slave_)
  {
    trigger_flag_ = field(name("SR"), "TIF");
  }
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
  mask_                = top >= 32 ? ~std::uint32_t{} : (std::uint32_t{1} << top) - 1u;
  auto const& instance = instance_;
  if(update_dma_)
  {
    update_signal_.emplace(ctx.peers, instance + "_upd_dma");
  }
  if(master_)
  {
    trgo_.emplace(ctx.peers, instance + "_trgo");
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
  for(auto const& [reg_name, bits] : std::array<std::pair<std::string_view, std::string_view>, 3>{
          {
           {"SMCR", "SMSPE"},
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
  return running() and state_.hz != 0;
}

auto tim::running() const -> bool
{
  return active() and internally_clocked();
}

auto tim::internally_clocked() const -> bool
{
  auto const mode = slave();
  if(mode == std::to_underlying(slave_mode::external1))
  {
    return false;
  }
  return gate_open();
}

auto tim::gate_open() const -> bool
{
  return not gating(slave()) or state_.gate != 0;
}

auto tim::counter_enabled() const -> bool
{
  return get(regs_, enable_) != 0 and gate_open();
}

auto tim::input_level(std::uint32_t smcr) const -> std::optional<bool>
{
  auto const input = input_of(smcr);
  if(not input)
  {
    return std::nullopt;
  }
  auto const  signal = std::format("{}_trgo", *input);
  auto const* source = trigger_source_of(ctx_.peers, signal);
  return source != nullptr ? source->level(signal) : std::nullopt;
}

auto tim::slave_of(std::uint32_t smcr) const -> std::uint32_t
{
  if(not slave_)
  {
    return 0;
  }
  auto const high = slave_high_ ? slave_high_->of(smcr) : 0u;
  return slave_->of(smcr) | (high << slave_->width);
}

auto tim::slave() const -> std::uint32_t
{
  return slave_ ? slave_of(regs_.peek(slave_->offset)) : 0u;
}

auto tim::trigger_select_of(std::uint32_t smcr) const -> std::uint32_t
{
  if(not select_)
  {
    return 0;
  }
  auto const high = select_high_ ? select_high_->of(smcr) : 0u;
  return select_->of(smcr) | (high << select_->width);
}

auto tim::input_of(std::uint32_t smcr) const -> std::optional<std::string_view>
{
  if(not modelled(slave_of(smcr)))
  {
    return std::nullopt;
  }
  auto const found = std::ranges::find(
      internal_triggers, trigger_select_of(smcr),
      &std::pair<std::uint32_t, std::string_view>::first);
  if(found == internal_triggers.end() or found->second == instance_)
  {
    return std::nullopt;
  }
  return found->second;
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
  if(state_.prescaler != get(regs_, prescaler_)
     or state_.reload != (regs_.peek(reload_.offset) & mask_))
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

auto tim::wants_update() -> bool
{
  if(get(regs_, no_update_) != 0)
  {
    return false;
  }
  auto const dma   = update_dma_ and get(regs_, *update_dma_) != 0 and state_.update_request == 0;
  auto const pulse = get(regs_, one_pulse_) != 0;
  return get(regs_, update_flag_) == 0 or dma or pulse or trgo_wanted(master_update);
}

auto tim::wants_match(std::size_t ch) -> bool
{
  auto const& c = channels_[ch];
  if(not output(ch) or compare_value(ch) > state_.reload)
  {
    return false;
  }
  auto const dma = c.dma and get(regs_, *c.dma) != 0 and state_.compare_request[ch] == 0;
  return get(regs_, c.flag) == 0 or dma or (ch == 0 and trgo_wanted(master_compare_pulse));
}

auto tim::master_mode() const -> std::uint32_t
{
  return master_ ? master_mode_of(regs_.peek(master_->offset)) : 0u;
}

auto tim::master_mode_of(std::uint32_t cr2) const -> std::uint32_t
{
  if(not master_)
  {
    return 0;
  }
  auto const high = master_high_ ? master_high_->of(cr2) : 0u;
  return master_->of(cr2) | (high << master_->width);
}

void tim::pulse_trgo(std::uint64_t at)
{
  auto const hz = ctx_.clock.hz(domain_);
  trgo_->pulse(at, hz == 0 ? 0 : (ps_per_second + hz - 1) / hz);
}

void tim::enable_edge(trigger_edge edge, std::uint64_t at)
{
  if(trgo_ and master_mode() == master_enable)
  {
    trgo_->edge(edge, at);
  }
}

auto tim::trgo_wanted(std::uint32_t mode) -> bool
{
  return trgo_ and master_mode() == mode and trgo_->wanted();
}

auto tim::at_ps(wide at) const -> std::uint64_t
{
  return static_cast<std::uint64_t>((at + state_.hz - 1) / state_.hz);
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

void tim::visit(std::uint64_t ticks, std::optional<std::uint64_t> at)
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
        auto const after = up ? value - count : count - value;
        match(ch, at ? *at : at_ps(anchor() + wide{after} * tick_unit()));
      }
    }
  }
  state_.count = static_cast<std::uint32_t>(up ? count + ticks : count - ticks);
}

void tim::wrap(std::uint64_t at)
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
        update_event(false, at);
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
        match(ch, at);
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
  auto const at = at_ps(anchor());
  if(updates and get(regs_, no_update_) == 0)
  {
    update_event(false, at);
  }
  auto const mode = center() ? get(regs_, *center_mode_) : 0;
  for(std::size_t ch = 0; ch < channels_.size(); ++ch)
  {
    auto const value   = compare_value(ch);
    auto const visited = value <= state_.reload
                     and not(mode == 1 and value == state_.reload)
                     and not(mode == 2 and value == 0);
    if(output(ch) and visited)
    {
      match(ch, at);
    }
  }
}

void tim::catch_up(std::uint64_t ps)
{
  reacting_scope const reacting{reacting_};
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
      if(trgo_wanted(master_update) or trgo_wanted(master_compare_pulse))
      {
        groups = 0;
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
    wrap(at_ps(anchor()));
  }
}

void tim::external_tick(std::uint64_t at)
{
  if(++state_.edges <= state_.prescaler)
  {
    return;
  }
  state_.edges = 0;
  if(ticks_to_wrap() > 1)
  {
    visit(1, at);
  }
  else
  {
    wrap(at);
  }
}

void tim::reinitialise(std::uint64_t at)
{
  if(trgo_ and master_mode() == master_reset)
  {
    pulse_trgo(at);
  }
  if(get(regs_, no_update_) == 0)
  {
    update_event(true, at);
  }
  state_.repetition = repetition_reload();
  state_.down       = 0;
  state_.edges      = 0;
  state_.count      = counting_up() or center() ? 0 : state_.reload;
  start_at(at);
}

void tim::begin(std::uint64_t at)
{
  if(get(regs_, enable_) != 0)
  {
    return;
  }
  auto const was = counter_enabled();
  update(regs_, enable_, 1);
  warn_unsupported();
  start_at(at);
  enable_changed(was, at);
}

void tim::gate(std::string_view signal, trigger_edge edge, bool closing_resets, std::uint64_t at)
{
  auto const* const source = trigger_source_of(ctx_.peers, signal);
  auto const        level  = source != nullptr ? source->level(signal) : std::nullopt;
  if(not level and edge == trigger_edge::rising and not std::exchange(gate_logged_, true))
  {
    warn(
        name("SMCR"), std::format(
                          "{}: gated mode needs a master in MMS 001, the counter holds",
                          reflex::enum_value_name(static_cast<slave_mode>(slave()))));
  }
  auto const open = level.value_or(false);
  if(open == (state_.gate != 0))
  {
    return;
  }
  auto const was = counter_enabled();
  state_.gate    = open ? 1 : 0;
  update(regs_, *trigger_flag_, 1);
  if(open)
  {
    start_at(at);
  }
  else if(closing_resets)
  {
    reinitialise(at);
  }
  enable_changed(was, at);
}

void tim::enable_changed(bool was, std::uint64_t at)
{
  auto const now = counter_enabled();
  if(now != was)
  {
    enable_edge(now ? trigger_edge::rising : trigger_edge::falling, at);
  }
}

void tim::update_event(bool software, std::uint64_t at)
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
  if(trgo_ and master_mode() == master_update)
  {
    pulse_trgo(at);
  }
  if(get(regs_, one_pulse_) != 0 and get(regs_, enable_) != 0)
  {
    auto const was = counter_enabled();
    update(regs_, enable_, 0);
    enable_changed(was, at);
  }
}

void tim::match(std::size_t ch, std::uint64_t at)
{
  auto& c = channels_[ch];
  update(regs_, c.flag, 1);
  if(c.dma and get(regs_, *c.dma) != 0 and state_.compare_request[ch] == 0)
  {
    state_.compare_request[ch] = 1;
    c.signal->set(true);
  }
  if(ch == 0 and trgo_ and master_mode() == master_compare_pulse)
  {
    pulse_trgo(at);
  }
}

void tim::capture(std::size_t ch, std::uint64_t at)
{
  auto const& c = channels_[ch];
  if(c.overcapture and get(regs_, c.flag) != 0)
  {
    update(regs_, *c.overcapture, 1);
  }
  regs_.poke(c.ccr.offset, (regs_.peek(c.ccr.offset) & ~mask_) | (state_.count & mask_));
  match(ch, at);
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
                     or state_.count < state_.reload
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
  auto const due = std::max(at_ps(anchor() + wide{*ticks} * tick_unit()), now_ps());
  if(state_.scheduled != 0 and state_.due_ps == due)
  {
    return;
  }
  state_.scheduled = 1;
  state_.due_ps    = due;
  ctx_.events.schedule_at(vtime{due}, tick_due, ++state_.generation, ctx_.clock);
}

void tim::warn_unsupported()
{
  if(unsupported_logged_)
  {
    return;
  }
  auto used = encoding(slave()) or std::ranges::any_of(unsupported_, [&](field_ref bits) {
                return get(regs_, bits) != 0;
              });
  for(std::size_t ch = 0; ch < channels_.size(); ++ch)
  {
    used = used or not output(ch);
  }
  if(used)
  {
    unsupported_logged_ = true;
    warn(
        name("CR1"),
        "the counter started with an encoder mode, SMSPE, an input capture channel, dithering or "
        "CCDS configured: not modelled, the time base counts on the internal clock");
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
  auto const ps     = now_ps();
  auto const word   = offset & ~3u;
  auto const smcr   = slave_ and word == slave_->offset;
  auto const before = smcr ? input_of(regs.peek(word)) : std::nullopt;
  auto const after =
      smcr ? input_of(merge_write(regs.peek(word), offset, size, value)) : std::nullopt;
  if(before)
  {
    static_cast<void>(stm32::interest_changed(ctx_.peers, std::format("{}_trgo", *before)));
  }
  if(after and after != before)
  {
    static_cast<void>(stm32::interest_changed(ctx_.peers, std::format("{}_trgo", *after)));
  }
  catch_up(ps);
  if(access.by == initiator::dma)
  {
    acknowledge();
  }
  auto const was     = running();
  auto const enabled = counter_enabled();
  auto       result  = std::expected<void, bus_error>{};
  {
    reacting_scope const reacting{reacting_};
    result = dispatch_model::on_write(regs, offset, size, old, value, access);
  }
  if(smcr)
  {
    slave_settled(before, regs.peek(word));
    reacting_scope const reacting{reacting_};
    enable_changed(enabled, ps);
  }
  if(not was and running())
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
  auto const hz = ctx_.clock.hz(domain_);
  if(hz == state_.hz)
  {
    return;
  }
  auto const ps    = now_ps();
  auto const woken = state_.hz == 0 and hz != 0;
  catch_up(ps);
  rebase(ps, hz);
  state_.scheduled = 0;
  if(woken and gating(slave()))
  {
    auto const was  = counter_enabled();
    auto const open = input_level(regs_.peek(slave_->offset)).value_or(false);
    if(open != (state_.gate != 0))
    {
      state_.gate = open ? 1 : 0;
      if(open)
      {
        start_at(ps);
      }
      else if(slave() == std::to_underlying(slave_mode::gated_reset))
      {
        reinitialise(ps);
      }
    }
    reacting_scope const reacting{reacting_};
    enable_changed(was, ps);
  }
  settle();
}

void tim::interest_changed()
{
  catch_up(now_ps());
  settle();
}

auto tim::produces(std::string_view signal) const -> bool
{
  return trgo_ and trgo_->signal() == signal;
}

auto tim::level(std::string_view signal) const -> std::optional<bool>
{
  if(not produces(signal) or master_mode() != master_enable)
  {
    return std::nullopt;
  }
  return counter_enabled();
}

auto tim::listens(std::string_view signal) const -> bool
{
  auto const input = slave_ ? input_of(regs_.peek(slave_->offset)) : std::nullopt;
  return input and signal.starts_with(*input) and signal.substr(input->size()) == "_trgo";
}

void tim::triggered(std::string_view signal, trigger_edge edge, std::uint64_t at_ps)
{
  if(not listens(signal) or ctx_.clock.hz(domain_) == 0)
  {
    return;
  }
  if(reacting_)
  {
    if(not std::exchange(cycle_logged_, true))
    {
      warn(name("SMCR"), std::format("trigger cycle through {}: edge dropped", desc_.name));
    }
    return;
  }
  reacting_scope const reacting{reacting_};
  catch_up(std::min(at_ps, now_ps()));
  auto const rising = edge == trigger_edge::rising;
  auto const mode   = static_cast<slave_mode>(slave());
  switch(mode)
  {
    case slave_mode::reset:
    case slave_mode::reset_trigger:
    case slave_mode::trigger:
      if(rising)
      {
        if(mode != slave_mode::trigger)
        {
          reinitialise(at_ps);
        }
        if(mode != slave_mode::reset)
        {
          begin(at_ps);
        }
        update(regs_, *trigger_flag_, 1);
      }
      break;
    case slave_mode::external1:
      if(rising)
      {
        update(regs_, *trigger_flag_, 1);
        if(active())
        {
          external_tick(at_ps);
        }
      }
      break;
    case slave_mode::gated:
    case slave_mode::gated_reset:
      gate(signal, edge, mode == slave_mode::gated_reset, at_ps);
      break;
    default:
      break;
  }
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
    enable_changed(false, now_ps());
  }
  if(enable_.of(old) != 0 and enable_.of(now) == 0)
  {
    state_.edges = 0;
    enable_changed(gate_open(), now_ps());
  }
}

void tim::master_written(std::uint32_t old, std::uint32_t now)
{
  auto const mode = master_mode();
  auto const was  = master_mode_of(old);
  if(trgo_ and counter_enabled() and (was == master_enable) != (mode == master_enable))
  {
    trgo_->edge(mode == master_enable ? trigger_edge::rising : trigger_edge::falling, now_ps());
  }
  auto const second = master2_ and master2_->of(now) != 0;
  if((mode > master_compare_pulse or second) and not std::exchange(master_logged_, true))
  {
    warn(
        name("CR2"),
        std::format(
            "MMS {:#06b}{}: master modes above 011 and TRGO2 are not modelled, no trigger "
            "pulses",
            mode, second ? " with MMS2" : ""));
  }
}

void tim::slave_written(std::uint32_t old, std::uint32_t now)
{
  if(slave_of(old) != slave_of(now))
  {
    state_.edges = 0;
  }
}

void tim::slave_settled(std::optional<std::string_view> before, std::uint32_t smcr)
{
  auto const after = input_of(smcr);
  if(before and before != after)
  {
    static_cast<void>(stm32::interest_changed(ctx_.peers, std::format("{}_trgo", *before)));
  }
  auto const signal = after ? std::format("{}_trgo", *after) : std::string{};
  auto const routed = after and stm32::interest_changed(ctx_.peers, signal);
  auto const mode   = slave_of(smcr);
  if(modelled(mode) and not routed and not std::exchange(unrouted_logged_, true))
  {
    warn(
        name("SMCR"),
        std::format(
            "SMS {:#06b} TS {}: the trigger input is not modelled, no trigger reaches the "
            "slave mode controller",
            mode, trigger_select_of(smcr)));
  }
  state_.gate = gating(mode) and input_level(smcr).value_or(false) ? 1 : 0;
}

void tim::interrupts_written(std::uint32_t, std::uint32_t now)
{
  if(trigger_dma_ and trigger_dma_->of(now) != 0 and not std::exchange(trigger_dma_logged_, true))
  {
    warn(name("DIER"), "TDE: the trigger DMA request is not modelled");
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
      match(ch, now_ps());
    }
    else
    {
      capture(ch, now_ps());
    }
  }
  if(update_generate_.of(now) != 0)
  {
    reinitialise(now_ps());
  }
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
