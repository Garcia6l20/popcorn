#include "dma.hpp"
#include "dma_data.hpp"
#include "factories.hpp"
#include "model_base.hpp"

#include <popcorn/core/clock.hpp>
#include <popcorn/core/errors.hpp>
#include <popcorn/core/services.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
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
constexpr std::size_t   max_channels   = 7;
constexpr std::size_t   max_requests   = 48;
constexpr std::uint32_t transfer_due   = 0;
constexpr std::uint64_t ps_per_second  = 1'000'000'000'000;
constexpr std::uint64_t latency_cycles = 4;

struct dma_classic_channel
{
  std::uint32_t peripheral{};
  std::uint32_t memory{};
  std::uint16_t programmed{};
  bool          half_reported{};

  friend auto operator==(dma_classic_channel const&, dma_classic_channel const&) -> bool = default;
};

struct dma_classic_state
{
  std::array<dma_classic_channel, max_channels> channel{};
  std::array<std::uint8_t, max_requests>        request{};
  std::uint32_t                                 generation{};
  bool                                          scheduled{};
  std::uint64_t                                 due_ps{};

  friend auto operator==(dma_classic_state const&, dma_classic_state const&) -> bool = default;
};

struct channel_fields
{
  reg_ref            ccr;
  reg_ref            cndtr;
  reg_ref            cpar;
  reg_ref            cmar;
  field_ref          enable;
  field_ref          complete_enable;
  field_ref          half_enable;
  field_ref          error_enable;
  field_ref          from_memory;
  field_ref          circular;
  field_ref          peripheral_increment;
  field_ref          memory_increment;
  field_ref          peripheral_size;
  field_ref          memory_size;
  field_ref          priority;
  field_ref          memory_to_memory;
  field_ref          count;
  field_ref          global;
  field_ref          complete;
  field_ref          half;
  field_ref          error;
  field_ref          clear_global;
  field_ref          clear_complete;
  field_ref          clear_half;
  field_ref          clear_error;
  irq_line*          line{};
  core::memory_grant read_grant{};
  core::memory_grant write_grant{};
};

enum class unsupported : std::uint8_t
{
  reserved_size,
  circular_memory,
};

/**
 * @brief Classic channel DMA of the STM32L1, DMA1 and DMA2, RM0038 section 11: 1 to 7 channels
 *        from the traits, `CCRx`, `CNDTRx`, `CPARx` and `CMARx` per channel, `ISR` and `IFCR`
 *        shared. A channel is ready while `EN` is set, `NDT` is not 0, and `MEM2MEM` is set or
 *        one of the hardware requests ORed into it is high. One transfer at a time runs for the
 *        whole controller, an event 4 HCLK cycles after the controller was last idle or after
 *        the previous transfer: the arbiter gives it to the ready channel of the highest `PL`,
 *        the lowest channel number among equals, RM0038 11.3.2, and re-arbitrates after every
 *        transfer, so a channel of higher priority enabled mid-copy takes the next transfer. No
 *        transfer runs inside a register or request callback. Each transfer reads one item of
 *        the source width at the source's current address and writes one item of the
 *        destination width, 0-padded when wider and its low bytes when narrower, RM0038 11.3.4
 *        Table 53. `DIR` 0 reads at the peripheral address and writes at the memory address,
 *        1 the other way, under `MEM2MEM` too. The current addresses start at `CPARx` and
 *        `CMARx` when `EN` goes from 0 to 1, are aligned down to their item size, RM0038 11.4.5
 *        and 11.4.6, and move by the item size under `PINC` and `MINC`. Writes to `CPARx` and
 *        `CMARx` while the channel runs reach the next enable or the next circular reload.
 *        `CNDTRx` takes writes only while `EN` is clear, RM0038 11.4.4, and counts down once per
 *        transfer. `HTIF` sets once per pass, when the items left are at most half the count
 *        latched at the enable, `TCIF` when they reach 0. Then a `CIRC` channel reloads the count
 *        and both addresses and runs on, any other keeps `EN` set and serves nothing until it is
 *        disabled and programmed again, RM0038 11.3.3. A read or write the bus refuses sets
 *        `TEIF` and clears `EN`, RM0038 11.3.5, with the count and the addresses left at the
 *        failed item. `GIFx` reads as the OR of `TEIFx`, `HTIFx` and `TCIFx`, inferred: RM0038
 *        11.4.1 sets it on any of them and clears it with `CGIFx`. A write of 1 to `CGIFx` clears
 *        all four flags, to `CTCIFx`, `CHTIFx` or `CTEIFx` one. The channel's
 *        `<instance>_Channel<n>_IRQn` is high while a flag is set with its enable bit, RM0038
 *        Table 54. A reserved `PSIZE` or `MSIZE` of 3 transfers words, and `CIRC` with
 *        `MEM2MEM`, which RM0038 11.3.3 forbids, runs the copy once: each logs one warning, a
 *        model_error under strict_peripherals, from the write that sets `EN`, or from the
 *        transfer for a `PSIZE` or `MSIZE` written while the channel runs. Every
 *        transfer goes through a core::memory_grant per channel for reads and one for writes,
 *        so the bus firewall is asked once per extent it allows rather than once per item. A
 *        hardware-requested transfer acknowledges every request of its channel that is high,
 *        and the end of a non-circular one tells every `dma_requester` peer each request of
 *        the channel. A request held high across a machine reset is cleared, one held across
 *        the RCC reset of the controller is kept.
 */
class dma_classic final : public dispatch_model<dma_classic, dma_classic_state>,
                          public dma_controller
{
public:
  dma_classic(
      model_context&                            ctx,
      peripheral_desc const&                    desc,
      dma_classic_traits const&                 traits,
      std::span<write_entry<dma_classic> const> table);

  void               reset(register_file& regs, reset_kind kind) override;
  void               on_event(std::uint32_t kind, std::uint64_t payload) override;
  void               on_request(std::uint32_t request, bool level) override;
  void               load(std::span<std::byte const> in) override;
  [[nodiscard]] auto request_of(std::string_view signal) const
      -> std::optional<std::uint32_t> override;

  template <std::size_t Ch> void control_written(std::uint32_t old, std::uint32_t now);
  template <std::size_t Ch> void count_written(std::uint32_t old, std::uint32_t now);
  void                           clear_written(std::uint32_t old, std::uint32_t now);

private:
  [[nodiscard]] auto ready(std::size_t ch) const -> bool;
  [[nodiscard]] auto requested(std::size_t ch) const -> bool;
  [[nodiscard]] auto winner() const -> std::optional<std::size_t>;
  [[nodiscard]] auto width(std::size_t ch, field_ref size) -> std::uint8_t;
  void               schedule();
  void               start(std::size_t ch);
  void               transfer(std::size_t ch);
  void               fail(std::size_t ch);
  void               raise(std::size_t ch, field_ref flag);
  void               interrupts(std::size_t ch);
  void               warn_once(std::size_t ch, unsupported what);
  void               drop_grants();
  auto               requesters() -> std::span<dma_requester* const>;

  register_file&                           regs_;
  dma_classic_traits                       traits_;
  std::array<channel_fields, max_channels> ch_{};
  core::clock_domain                       hclk_;
  std::array<bool, 2>                      unsupported_logged_{};
  bool                                     stopped_clock_logged_{};
  std::vector<dma_requester*>              requesters_;
  bool                                     requesters_resolved_{};
};

template <std::size_t... Ch>
auto channel_entries(std::index_sequence<Ch...>, std::deque<std::string>& names)
    -> std::vector<write_entry<dma_classic>>
{
  std::vector<write_entry<dma_classic>> out;
  auto const add = [&](std::string name,
                       void (dma_classic::*handler)(std::uint32_t, std::uint32_t)) {
    names.push_back(std::move(name));
    out.push_back({names.back(), handler});
  };
  add("ISR", nullptr);
  add("IFCR", &dma_classic::clear_written);
  (
      [&] {
        add(std::format("CCR{}", Ch + 1), &dma_classic::control_written<Ch>);
        add(std::format("CNDTR{}", Ch + 1), &dma_classic::count_written<Ch>);
        add(std::format("CPAR{}", Ch + 1), nullptr);
        add(std::format("CMAR{}", Ch + 1), nullptr);
      }(),
      ...);
  return out;
}

template <std::size_t Channels> auto writes() -> std::span<write_entry<dma_classic> const>
{
  static std::deque<std::string>                     names;
  static std::vector<write_entry<dma_classic>> const table =
      channel_entries(std::make_index_sequence<Channels>{}, names);
  return table;
}

template <std::size_t... Ch>
auto writes_by_count(std::size_t channels, std::index_sequence<Ch...>)
    -> std::span<write_entry<dma_classic> const>
{
  std::span<write_entry<dma_classic> const> out;
  static_cast<void>(((channels == Ch + 1 ? (out = writes<Ch + 1>(), true) : false) or ...));
  return out;
}

dma_classic::dma_classic(
    model_context&                            ctx,
    peripheral_desc const&                    desc,
    dma_classic_traits const&                 traits,
    std::span<write_entry<dma_classic> const> table)
    : dispatch_model{ctx, desc, "DMA", table}, regs_{ctx.regs}, traits_{traits},
      hclk_{ctx.clock.domain("hclk")}
{
  for(std::size_t ch = 0; ch < traits_.channels; ++ch)
  {
    auto const n           = ch + 1;
    auto const ccr         = std::format("CCR{}", n);
    auto&      c           = ch_[ch];
    c.ccr                  = reg(ccr);
    c.cndtr                = reg(std::format("CNDTR{}", n));
    c.cpar                 = reg(std::format("CPAR{}", n));
    c.cmar                 = reg(std::format("CMAR{}", n));
    c.enable               = field(ccr, "EN");
    c.complete_enable      = field(ccr, "TCIE");
    c.half_enable          = field(ccr, "HTIE");
    c.error_enable         = field(ccr, "TEIE");
    c.from_memory          = field(ccr, "DIR");
    c.circular             = field(ccr, "CIRC");
    c.peripheral_increment = field(ccr, "PINC");
    c.memory_increment     = field(ccr, "MINC");
    c.peripheral_size      = field(ccr, "PSIZE");
    c.memory_size          = field(ccr, "MSIZE");
    c.priority             = field(ccr, "PL");
    c.memory_to_memory     = field(ccr, "MEM2MEM");
    c.count                = field(std::format("CNDTR{}", n), "NDT");
    c.global               = field("ISR", std::format("GIF{}", n));
    c.complete             = field("ISR", std::format("TCIF{}", n));
    c.half                 = field("ISR", std::format("HTIF{}", n));
    c.error                = field("ISR", std::format("TEIF{}", n));
    c.clear_global         = field("IFCR", std::format("CGIF{}", n));
    c.clear_complete       = field("IFCR", std::format("CTCIF{}", n));
    c.clear_half           = field("IFCR", std::format("CHTIF{}", n));
    c.clear_error          = field("IFCR", std::format("CTEIF{}", n));
    c.line                 = &irq(std::format("{}_Channel{}_IRQn", desc.name, n));
  }
  if(traits_.channels < max_channels)
  {
    expect_register(std::format("CCR{}", traits_.channels + 1), false);
    expect_field("ISR", std::format("GIF{}", traits_.channels + 1), false);
  }
}

void dma_classic::reset(register_file&, reset_kind kind)
{
  drop_grants();
  for(auto const& name : claims())
  {
    regs_.poke(reg(name).offset, describe(name).reset);
  }
  auto const requests = state_.request;
  auto const next     = state_.generation + 1;
  state_              = {};
  state_.generation   = next;
  if(kind == reset_kind::peripheral)
  {
    state_.request = requests;
  }
  for(std::size_t ch = 0; ch < traits_.channels; ++ch)
  {
    interrupts(ch);
  }
}

void dma_classic::load(std::span<std::byte const> in)
{
  dispatch_model::load(in);
  drop_grants();
}

void dma_classic::drop_grants()
{
  for(auto& c : ch_)
  {
    c.read_grant  = {};
    c.write_grant = {};
  }
}

auto dma_classic::requested(std::size_t ch) const -> bool
{
  for(std::size_t r = 0; r < traits_.requests.size() and r < max_requests; ++r)
  {
    if(traits_.requests[r].channel == ch + 1 and state_.request[r] != 0)
    {
      return true;
    }
  }
  return false;
}

auto dma_classic::ready(std::size_t ch) const -> bool
{
  auto const& c = ch_[ch];
  return get(regs_, c.enable) != 0
     and get(regs_, c.count) != 0
     and (get(regs_, c.memory_to_memory) != 0 or requested(ch));
}

auto dma_classic::winner() const -> std::optional<std::size_t>
{
  std::optional<std::size_t> best;
  for(std::size_t ch = 0; ch < traits_.channels; ++ch)
  {
    if(ready(ch) and (not best or get(regs_, ch_[ch].priority) > get(regs_, ch_[*best].priority)))
    {
      best = ch;
    }
  }
  return best;
}

void dma_classic::schedule()
{
  if(state_.scheduled or not winner())
  {
    return;
  }
  auto const hz = ctx_.clock.hz(hclk_);
  if(hz == 0)
  {
    if(not std::exchange(stopped_clock_logged_, true))
    {
      warn("CCR1", "HCLK is stopped: the transfer waits");
    }
    return;
  }
  state_.scheduled = true;
  state_.due_ps    = ctx_.clock.time().ps + latency_cycles * ps_per_second / hz;
  ctx_.events.schedule_at(vtime{state_.due_ps}, transfer_due, state_.generation, ctx_.clock);
}

void dma_classic::on_event(std::uint32_t kind, std::uint64_t payload)
{
  if(kind != transfer_due or payload != state_.generation or not state_.scheduled)
  {
    return;
  }
  state_.scheduled = false;
  if(auto const ch = winner())
  {
    transfer(*ch);
  }
  schedule();
}

void dma_classic::warn_once(std::size_t ch, unsupported what)
{
  constexpr std::array<std::string_view, 2> texts{
      "a reserved PSIZE or MSIZE of 3: the channel transfers words",
      "CIRC with MEM2MEM, which RM0038 11.3.3 forbids: the copy runs once"};
  auto const index = static_cast<std::size_t>(what);
  if(not std::exchange(unsupported_logged_[index], true))
  {
    warn(std::format("CCR{}", ch + 1), std::format("channel {} uses {}", ch + 1, texts[index]));
  }
  if(ctx_.strict_peripherals)
  {
    throw model_error(std::string{desc_.name}, std::format("CCR{}", ch + 1), 0);
  }
}

auto dma_classic::width(std::size_t ch, field_ref size) -> std::uint8_t
{
  auto const code = get(regs_, size);
  if(code == 3)
  {
    warn_once(ch, unsupported::reserved_size);
    return 4;
  }
  return static_cast<std::uint8_t>(1u << code);
}

void dma_classic::start(std::size_t ch)
{
  auto const& c   = ch_[ch];
  auto&       s   = state_.channel[ch];
  s.peripheral    = regs_.peek(c.cpar.offset);
  s.memory        = regs_.peek(c.cmar.offset);
  s.programmed    = static_cast<std::uint16_t>(get(regs_, c.count));
  s.half_reported = false;
  static_cast<void>(width(ch, c.peripheral_size));
  static_cast<void>(width(ch, c.memory_size));
  if(get(regs_, c.circular) != 0 and get(regs_, c.memory_to_memory) != 0)
  {
    warn_once(ch, unsupported::circular_memory);
  }
}

void dma_classic::transfer(std::size_t ch)
{
  auto&      c           = ch_[ch];
  auto&      s           = state_.channel[ch];
  auto const psize       = width(ch, c.peripheral_size);
  auto const msize       = width(ch, c.memory_size);
  auto const from_memory = get(regs_, c.from_memory) != 0;
  auto const source      = from_memory ? s.memory & ~(msize - 1u) : s.peripheral & ~(psize - 1u);
  auto const target      = from_memory ? s.peripheral & ~(psize - 1u) : s.memory & ~(msize - 1u);
  auto const read_size   = from_memory ? msize : psize;
  auto const write_size  = from_memory ? psize : msize;
  constexpr bus_attrs by_dma{.by = initiator::dma};
  auto const          value = ctx_.services.read(source, read_size, by_dma, c.read_grant);
  if(not value
     or not ctx_.services.write(
         target, write_size, resize(*value, read_size, write_size, 0), by_dma, c.write_grant))
  {
    fail(ch);
    return;
  }
  if(get(regs_, c.peripheral_increment) != 0)
  {
    s.peripheral += psize;
  }
  if(get(regs_, c.memory_increment) != 0)
  {
    s.memory += msize;
  }
  auto const left     = get(regs_, c.count) - 1u;
  auto const hardware = get(regs_, c.memory_to_memory) == 0;
  update(regs_, c.count, left);
  if(hardware)
  {
    for(std::size_t r = 0; r < traits_.requests.size() and r < max_requests; ++r)
    {
      if(traits_.requests[r].channel == ch + 1 and state_.request[r] != 0)
      {
        for(auto* const requester : requesters())
        {
          requester->acknowledged(*this, static_cast<std::uint32_t>(r));
        }
      }
    }
  }
  if(not s.half_reported and left * 2u <= s.programmed)
  {
    s.half_reported = true;
    raise(ch, c.half);
  }
  if(left != 0)
  {
    return;
  }
  raise(ch, c.complete);
  if(get(regs_, c.circular) != 0 and hardware)
  {
    update(regs_, c.count, s.programmed);
    s.peripheral    = regs_.peek(c.cpar.offset);
    s.memory        = regs_.peek(c.cmar.offset);
    s.half_reported = false;
    return;
  }
  if(not hardware)
  {
    return;
  }
  for(std::size_t r = 0; r < traits_.requests.size() and r < max_requests; ++r)
  {
    if(traits_.requests[r].channel == ch + 1)
    {
      for(auto* const requester : requesters())
      {
        requester->transfer_completed(*this, static_cast<std::uint32_t>(r));
      }
    }
  }
}

void dma_classic::fail(std::size_t ch)
{
  update(regs_, ch_[ch].enable, 0);
  raise(ch, ch_[ch].error);
}

void dma_classic::raise(std::size_t ch, field_ref flag)
{
  update(regs_, flag, 1);
  interrupts(ch);
}

void dma_classic::interrupts(std::size_t ch)
{
  auto const& c        = ch_[ch];
  auto const  complete = get(regs_, c.complete) != 0;
  auto const  half     = get(regs_, c.half) != 0;
  auto const  error    = get(regs_, c.error) != 0;
  update(regs_, c.global, complete or half or error ? 1 : 0);
  auto const level = (complete and get(regs_, c.complete_enable) != 0)
                  or (half and get(regs_, c.half_enable) != 0)
                  or (error and get(regs_, c.error_enable) != 0);
  if(level)
  {
    c.line->raise();
  }
  else
  {
    c.line->lower();
  }
}

template <std::size_t Ch> void dma_classic::control_written(std::uint32_t old, std::uint32_t now)
{
  auto const& c = ch_[Ch];
  if(c.enable.of(now) != 0 and c.enable.of(old) == 0)
  {
    start(Ch);
  }
  interrupts(Ch);
  schedule();
}

template <std::size_t Ch> void dma_classic::count_written(std::uint32_t old, std::uint32_t)
{
  if(get(regs_, ch_[Ch].enable) != 0)
  {
    regs_.poke(ch_[Ch].cndtr.offset, old);
  }
}

void dma_classic::clear_written(std::uint32_t, std::uint32_t now)
{
  for(std::size_t ch = 0; ch < traits_.channels; ++ch)
  {
    auto const& c   = ch_[ch];
    auto const  all = c.clear_global.of(now) != 0;
    if(all or c.clear_complete.of(now) != 0)
    {
      update(regs_, c.complete, 0);
    }
    if(all or c.clear_half.of(now) != 0)
    {
      update(regs_, c.half, 0);
    }
    if(all or c.clear_error.of(now) != 0)
    {
      update(regs_, c.error, 0);
    }
    interrupts(ch);
  }
  regs_.poke(ch_[0].clear_global.offset, 0);
}

auto dma_classic::requesters() -> std::span<dma_requester* const>
{
  if(not requesters_resolved_ and not ctx_.peers.all().empty())
  {
    requesters_resolved_ = true;
    for(auto* const peer : ctx_.peers.all())
    {
      if(auto* const requester = dynamic_cast<dma_requester*>(peer); requester != nullptr)
      {
        requesters_.push_back(requester);
      }
    }
  }
  return requesters_;
}

auto dma_classic::request_of(std::string_view signal) const -> std::optional<std::uint32_t>
{
  auto const found = std::ranges::find(traits_.requests, signal, &dma_classic_request::signal);
  if(found == traits_.requests.end())
  {
    return std::nullopt;
  }
  return static_cast<std::uint32_t>(found - traits_.requests.begin());
}

void dma_classic::on_request(std::uint32_t request, bool level)
{
  if(request >= max_requests or request >= traits_.requests.size())
  {
    return;
  }
  state_.request[request] = level ? 1 : 0;
  if(level)
  {
    schedule();
  }
}
}

auto dma_classic_factory(dma_classic_traits traits) -> model_factory
{
  return [traits](model_context& ctx, peripheral_desc const& desc) -> std::shared_ptr<model> {
    auto const table = writes_by_count(traits.channels, std::make_index_sequence<max_channels>{});
    return std::make_shared<dma_classic>(ctx, desc, traits, table);
  };
}
}
