#include "dma.hpp"
#include "dma_data.hpp"
#include "dma_rows.hpp"
#include "model_base.hpp"

#include <popcorn/core/clock.hpp>
#include <popcorn/core/errors.hpp>
#include <popcorn/core/services.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
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
constexpr std::size_t   max_requests   = 64;
constexpr std::uint32_t transfer_due   = 0;
constexpr std::uint64_t latency_cycles = 4;

struct dma_l1l4_channel
{
  std::uint32_t peripheral{};
  std::uint32_t memory{};
  std::uint16_t programmed{};
  bool          half_reported{};

  friend auto operator==(dma_l1l4_channel const&, dma_l1l4_channel const&) -> bool = default;
};

struct dma_l1l4_state
{
  std::array<dma_l1l4_channel, max_channels> channel{};
  std::array<std::uint8_t, max_requests>     request{};
  std::uint32_t                              generation{};
  bool                                       scheduled{};
  std::uint64_t                              due_ps{};

  friend auto operator==(dma_l1l4_state const&, dma_l1l4_state const&) -> bool = default;
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
  field_ref          selection{};
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
 * @brief Classic channel DMA of the STM32L1, DMA1 and DMA2, RM0038 section 11: 1 to 7 channels from
 *        the variant, `CCRx`, `CNDTRx`, `CPARx` and `CMARx` per channel, `ISR` and `IFCR` shared. A
 *        channel is ready while `EN` is set, `NDT` is not 0, and `MEM2MEM` is set or one of the
 *        hardware requests ORed into it is high. One transfer at a time runs for the whole
 *        controller, an event 4 HCLK cycles after the controller was last idle or after the
 *        previous transfer: the arbiter gives it to the ready channel of the highest `PL`, the
 *        lowest channel number among equals, RM0038 11.3.2, and re-arbitrates after every transfer,
 *        so a channel of higher priority enabled mid-copy takes the next transfer. No transfer runs
 *        inside a register or request callback. Each transfer reads one item of the source width at
 *        the source's current address and writes one item of the destination width, 0-padded when
 *        wider and its low bytes when narrower, RM0038 11.3.4 Table 53. A byte or halfword write
 *        into an APB window of the variant writes the word at the address aligned down to 4, with
 *        the item repeated on every lane, as the AHB/APB bridge of RM0038 11.3.4 does. `DIR` 0
 *        reads at the peripheral address and writes at the memory address, 1 the other way, under
 *        `MEM2MEM` too. The current addresses start at `CPARx` and `CMARx` when `EN` goes from 0 to
 *        1, are aligned down to their item size, RM0038 11.4.5 and 11.4.6, and move by the item
 *        size under `PINC` and `MINC`. Writes to `CPARx` and `CMARx` while the channel runs reach
 *        the next enable or the next circular reload. `CNDTRx` takes writes only while `EN` is
 *        clear, RM0038 11.4.4, and counts down once per transfer. `HTIF` sets once per pass, when
 *        the items left are at most half the count latched at the enable, `TCIF` when they reach 0.
 *        Then a `CIRC` channel reloads the count and both addresses and runs on, any other keeps
 *        `EN` set and serves nothing until it is disabled and programmed again, RM0038 11.3.3. A
 *        read or write the bus refuses sets `TEIF` and clears `EN`, RM0038 11.3.5, with the count
 *        and the addresses left at the failed item. `GIFx` reads as the OR of `TEIFx`, `HTIFx` and
 *        `TCIFx`, inferred: RM0038 11.4.1 sets it on any of them and clears it with `CGIFx`. A
 *        write of 1 to `CGIFx` clears all four flags, to `CTCIFx`, `CHTIFx` or `CTEIFx` one. The
 *        channel's `<instance>_Channel<n>_IRQn` is high while a flag is set with its enable bit,
 *        RM0038 Table 54. A reserved `PSIZE` or `MSIZE` of 3 transfers words, and `CIRC` with
 *        `MEM2MEM`, which RM0038 11.3.3 forbids, runs the copy once: each logs one warning, a
 *        model_error under strict_peripherals, from the write that sets `EN`, or from the transfer
 *        for a `PSIZE` or `MSIZE` written while the channel runs. Every transfer goes through a
 *        core::memory_grant per channel for reads and one for writes, so the bus firewall is asked
 *        once per extent it allows rather than once per item. A hardware-requested transfer
 *        acknowledges every request of its channel that is high, and the end of a non-circular one
 *        tells every `dma_requester` peer each request of the channel, inferred. A
 *        hardware-requested transfer that leaves one item tells every `dma_requester` peer each
 *        request of the channel through dma_requester::last_but_one(), the EOT_1 signal of RM0038
 *        26.3.7, once per pass of a circular channel. The last item of a hardware-requested count
 *        tells every peer each request of the channel through dma_requester::count_ended(), at
 *        the end of a normal channel and at each circular reload. A request held high across a
 *        machine reset is cleared, one held across the RCC reset of the controller is kept.
 *        The STM32L47x/L48x DMA1 and DMA2, RM0351 section 11, add DMA_CSELR: a request of RM0351
 *        Tables 46 and 47 is ORed into its channel only while the channel's C<n>S holds the
 *        request's code, and a CSELR write re-arbitrates. A signal the tables list on two
 *        channels raises its row on each, and its requester sees the number of its first row.
 *        A firmware byte or halfword write to a register logs one model_warning per controller
 *        and raises model_error under strict_peripherals before it is stored, popcorn policy
 *        after RM0351 11.6, "The DMA registers have to be accessed by words (32-bit)". Without
 *        strict the write lands as the bus merges it.
 *
 * @limits dma DMA
 *
 * @group l1,l4 rule
 * - STM32L1 DMA1 and DMA2, RM0038 section 11. One transfer runs at a time per controller, 4 HCLK
 *   cycles after the previous one, inferred. RM0038 gives no transfer time.
 * - `GIFx` reads as the OR of `TEIFx`, `HTIFx` and `TCIFx`, inferred.
 * - `HTIFx` sets once per pass, when the items left are at most half the count latched at the
 *   enable. For an odd count the rounding is inferred.
 * - `CPARx` and `CMARx` take writes while the channel runs, and the running transfer keeps the
 *   addresses it latched at the enable until the next circular reload reads them again. `CCRx`
 *   fields take writes while the channel runs and apply from the next item, inferred. `CNDTRx`
 *   ignores writes while `EN` is set, RM0038 11.4.4.
 * - A transfer error leaves the count and the current addresses at the failed item, inferred. A
 *   circular channel disabled mid-copy and enabled again with no `CNDTRx` write restarts from the
 *   count left at the disable, inferred.
 * - A reserved `PSIZE` or `MSIZE` of 3 transfers words, inferred from RM0038 11.4.3, and `CIRC`
 *   with `MEM2MEM` runs the copy once, inferred from RM0038 11.3.3. Each logs one `model_warning`.
 * - A byte or halfword DMA write to an APB1 or APB2 address is a word write at the address aligned
 *   down to 4, with the item repeated on every lane, RM0038 11.3.4. A CPU byte or halfword store to
 *   the same address writes its own lanes only, inferred.
 * - The requests of a channel are ORed, RM0038 11.3.7. Each item acknowledges every request of the
 *   channel that is high, and the end of a non-circular transfer tells every request of the
 *   channel, inferred.
 * - A peripheral-requested transfer that leaves one item tells every request of the channel, once
 *   per pass of a circular channel. This is the EOT_1 signal the I2C reads with CR2.LAST, RM0038
 *   26.3.7. A memory-to-memory copy tells none.
 * - The channels run whatever `RCC_AHBENR.DMAxEN` holds, which gates only their register access,
 *   see RCC.
 *
 * @group l4 rule
 * - STM32L4 DMA1 and DMA2, RM0351 section 11, follow the STM32L1 rules above, the inferred ones
 *   inferred on the STM32L4 too, with the APB windows of RM0351 Table 1 and `RCC_AHB1ENR`.
 * - STM32L4: `DMA_CSELR` routes each request of RM0351 Tables 46 and 47 to its channel while the
 *   channel's `CxS` holds the request's code. A request with another code reaches nothing. A signal
 *   the tables list on two channels, such as `SAI1_A` on DMA2 channels 1 and 6, reaches both. The
 *   I2C4, DCMI and HASH requests of the STM32L49x/L4Ax have no row.
 * - STM32L4: a firmware byte or halfword write to a DMA register logs one `model_warning` per
 *   controller and, under `strict`, raises `ModelError` before the store, after RM0351 11.6 "The
 *   DMA registers have to be accessed by words (32-bit)". Without `strict` it lands. ES0250 2.4.2
 *   states the same as a description correction.
 *
 * @group l4 note
 * - STM32L4: ES0250 2.4.1. A transfer error sets `TEIFx` and clears `EN` whatever `CGIFx` the
 *   firmware writes in the same cycle.
 * - STM32L4: ES0250 2.8.14, 2.8.16 and 2.8.17. The ADC2 and ADC3 requests on DMA2 channels 4 and 5
 *   need no request 0 on channels 2 and 3, and DMA2 channels 4 and 5 hand their acknowledge and end
 *   of transfer to their own requests only, while DMA1 serves ADC2 or ADC3.
 */
class dma_l1l4 final : public dispatch_model<dma_l1l4, dma_l1l4_state>, public dma_controller
{
public:
  dma_l1l4(
      model_context&                         ctx,
      peripheral_desc const&                 desc,
      dma_l1l4_variant const&                variant,
      std::span<write_entry<dma_l1l4> const> table);

  void reset(register_file& regs, reset_kind kind) override;
  void on_event(std::uint32_t kind, std::uint64_t payload) override;
  void on_request(std::uint32_t request, bool level) override;
  void load(std::span<std::byte const> in) override;
  auto on_write(
      register_file& regs,
      std::uint32_t  offset,
      std::uint8_t   size,
      std::uint32_t  old,
      std::uint32_t  value,
      access_ctx     ctx) -> std::expected<void, bus_error> override;
  [[nodiscard]] auto request_of(std::string_view signal) const
      -> std::optional<std::uint32_t> override;

  template <std::size_t Ch> void control_written(std::uint32_t old, std::uint32_t now);
  template <std::size_t Ch> void count_written(std::uint32_t old, std::uint32_t now);
  void                           clear_written(std::uint32_t old, std::uint32_t now);
  void                           selection_written(std::uint32_t old, std::uint32_t now);

private:
  [[nodiscard]] auto ready(std::size_t ch) const -> bool;
  [[nodiscard]] auto requested(std::size_t ch) const -> bool;
  [[nodiscard]] auto routed(std::size_t r, std::size_t ch) const -> bool;
  [[nodiscard]] auto first_of(std::size_t r) const -> std::uint32_t;
  [[nodiscard]] auto winner() const -> std::optional<std::size_t>;
  [[nodiscard]] auto width(std::size_t ch, field_ref size) -> std::uint8_t;
  [[nodiscard]] auto on_apb(std::uint32_t at) const -> bool;
  void               schedule(std::uint64_t base_ps);
  void               start(std::size_t ch);
  void               transfer(std::size_t ch);
  void               fail(std::size_t ch);
  void tell(std::size_t ch, void (dma_requester::*signal)(dma_controller const&, std::uint32_t));
  void raise(std::size_t ch, field_ref flag);
  void interrupts(std::size_t ch);
  void warn_once(std::size_t ch, unsupported what);
  void drop_grants();

  register_file&                           regs_;
  dma_l1l4_variant                         variant_;
  std::array<channel_fields, max_channels> ch_{};
  core::clock_domain                       hclk_;
  std::array<bool, 2>                      unsupported_logged_{};
  bool                                     stopped_clock_logged_{};
  bool                                     narrow_logged_{};
  dma_requester_list                       requesters_;
};

template <std::size_t... Ch>
auto channel_entries(std::index_sequence<Ch...>, std::deque<std::string>& names)
    -> std::vector<write_entry<dma_l1l4>>
{
  std::vector<write_entry<dma_l1l4>> out;
  auto const add = [&](std::string name, void (dma_l1l4::*handler)(std::uint32_t, std::uint32_t)) {
    names.push_back(std::move(name));
    out.push_back({names.back(), handler});
  };
  add("ISR", nullptr);
  add("IFCR", &dma_l1l4::clear_written);
  (
      [&] {
        add(std::format("CCR{}", Ch + 1), &dma_l1l4::control_written<Ch>);
        add(std::format("CNDTR{}", Ch + 1), &dma_l1l4::count_written<Ch>);
        add(std::format("CPAR{}", Ch + 1), nullptr);
        add(std::format("CMAR{}", Ch + 1), nullptr);
      }(),
      ...);
  return out;
}

template <std::size_t Channels> auto writes() -> std::span<write_entry<dma_l1l4> const>
{
  static std::deque<std::string>                  names;
  static std::vector<write_entry<dma_l1l4>> const table =
      channel_entries(std::make_index_sequence<Channels>{}, names);
  return table;
}

template <std::size_t... Ch>
auto writes_by_count(std::size_t channels, std::index_sequence<Ch...>)
    -> std::span<write_entry<dma_l1l4> const>
{
  std::span<write_entry<dma_l1l4> const> out;
  static_cast<void>(((channels == Ch + 1 ? (out = writes<Ch + 1>(), true) : false) or ...));
  return out;
}

dma_l1l4::dma_l1l4(
    model_context&                         ctx,
    peripheral_desc const&                 desc,
    dma_l1l4_variant const&                variant,
    std::span<write_entry<dma_l1l4> const> table)
    : dispatch_model{ctx, desc, "DMA", table}, regs_{ctx.regs}, variant_{variant},
      hclk_{ctx.clock.domain("hclk")}
{
  auto const unserved = std::ranges::any_of(variant_.requests, [&](dma_l1l4_request const& row) {
    return row.channel == 0 or row.channel > variant_.channels;
  });
  if(variant_.channels == 0
     or variant_.channels > max_channels
     or variant_.requests.size() > max_requests
     or unserved)
  {
    throw config_error(
        desc.name, 0, "variant",
        std::format(
            "1 to {} channels and at most {} requests, each on one of the channels", max_channels,
            max_requests));
  }
  for(std::size_t ch = 0; ch < variant_.channels; ++ch)
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
    if(not variant_.selection_register.empty())
    {
      c.selection = field(variant_.selection_register, std::format("C{}S", n));
    }
  }
  if(variant_.channels < max_channels)
  {
    expect_register(std::format("CCR{}", variant_.channels + 1), false);
    expect_field("ISR", std::format("GIF{}", variant_.channels + 1), false);
  }
}

void dma_l1l4::reset(register_file&, reset_kind kind)
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
  for(std::size_t ch = 0; ch < variant_.channels; ++ch)
  {
    interrupts(ch);
  }
}

void dma_l1l4::load(std::span<std::byte const> in)
{
  dispatch_model::load(in);
  drop_grants();
}

void dma_l1l4::drop_grants()
{
  for(auto& c : ch_)
  {
    c.read_grant  = {};
    c.write_grant = {};
  }
}

auto dma_l1l4::routed(std::size_t r, std::size_t ch) const -> bool
{
  auto const& row = variant_.requests[r];
  return row.channel == ch + 1
     and (ch_[ch].selection.width == 0 or get(regs_, ch_[ch].selection) == row.code);
}

auto dma_l1l4::first_of(std::size_t r) const -> std::uint32_t
{
  auto const found =
      std::ranges::find(variant_.requests, variant_.requests[r].signal, &dma_l1l4_request::signal);
  return static_cast<std::uint32_t>(found - variant_.requests.begin());
}

auto dma_l1l4::requested(std::size_t ch) const -> bool
{
  for(std::size_t r = 0; r < variant_.requests.size() and r < max_requests; ++r)
  {
    if(routed(r, ch) and state_.request[r] != 0)
    {
      return true;
    }
  }
  return false;
}

auto dma_l1l4::ready(std::size_t ch) const -> bool
{
  auto const& c = ch_[ch];
  return get(regs_, c.enable) != 0
     and get(regs_, c.count) != 0
     and (get(regs_, c.memory_to_memory) != 0 or requested(ch));
}

auto dma_l1l4::winner() const -> std::optional<std::size_t>
{
  std::optional<std::size_t> best;
  for(std::size_t ch = 0; ch < variant_.channels; ++ch)
  {
    if(ready(ch) and (not best or get(regs_, ch_[ch].priority) > get(regs_, ch_[*best].priority)))
    {
      best = ch;
    }
  }
  return best;
}

void dma_l1l4::schedule(std::uint64_t base_ps)
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
  state_.due_ps    = base_ps + latency_cycles * ps_per_second / hz;
  ctx_.events.schedule_at(vtime{state_.due_ps}, transfer_due, state_.generation, ctx_.clock);
}

void dma_l1l4::on_event(std::uint32_t kind, std::uint64_t payload)
{
  if(kind != transfer_due or payload != state_.generation or not state_.scheduled)
  {
    return;
  }
  state_.scheduled = false;
  auto const base  = state_.due_ps;
  if(auto const ch = winner())
  {
    transfer(*ch);
  }
  schedule(base);
}

void dma_l1l4::warn_once(std::size_t ch, unsupported what)
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

auto dma_l1l4::width(std::size_t ch, field_ref size) -> std::uint8_t
{
  auto const code = get(regs_, size);
  if(code == 3)
  {
    warn_once(ch, unsupported::reserved_size);
    return 4;
  }
  return static_cast<std::uint8_t>(1u << code);
}

auto dma_l1l4::on_apb(std::uint32_t at) const -> bool
{
  return std::ranges::any_of(variant_.apb_windows, [at](bus_window const& window) {
    return at >= window.first and at <= window.last;
  });
}

void dma_l1l4::start(std::size_t ch)
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

void dma_l1l4::transfer(std::size_t ch)
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
  if(not value)
  {
    fail(ch);
    return;
  }
  auto item = static_cast<std::uint32_t>(resize(*value, read_size, write_size, 0));
  auto at   = target;
  auto size = write_size;
  if(size < 4 and on_apb(target))
  {
    item = size == 1 ? item * 0x0101'0101u : item * 0x0001'0001u;
    at   = target & ~3u;
    size = 4;
  }
  if(not ctx_.services.write(at, size, item, by_dma, c.write_grant))
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
    for(std::size_t r = 0; r < variant_.requests.size() and r < max_requests; ++r)
    {
      if(routed(r, ch) and state_.request[r] != 0)
      {
        for(auto* const requester : requesters_.resolve(ctx_.peers))
        {
          requester->acknowledged(*this, first_of(r));
        }
      }
    }
    if(left == 1)
    {
      tell(ch, &dma_requester::last_but_one);
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
  if(hardware)
  {
    tell(ch, &dma_requester::count_ended);
  }
  if(get(regs_, c.circular) != 0 and hardware)
  {
    update(regs_, c.count, s.programmed);
    s.peripheral    = regs_.peek(c.cpar.offset);
    s.memory        = regs_.peek(c.cmar.offset);
    s.half_reported = false;
    return;
  }
  if(hardware)
  {
    tell(ch, &dma_requester::transfer_completed);
  }
}

void dma_l1l4::tell(
    std::size_t ch,
    void (dma_requester::*signal)(dma_controller const&, std::uint32_t))
{
  for(std::size_t r = 0; r < variant_.requests.size() and r < max_requests; ++r)
  {
    if(routed(r, ch))
    {
      for(auto* const requester : requesters_.resolve(ctx_.peers))
      {
        (requester->*signal)(*this, first_of(r));
      }
    }
  }
}

void dma_l1l4::fail(std::size_t ch)
{
  update(regs_, ch_[ch].enable, 0);
  raise(ch, ch_[ch].error);
}

void dma_l1l4::raise(std::size_t ch, field_ref flag)
{
  update(regs_, flag, 1);
  interrupts(ch);
}

void dma_l1l4::interrupts(std::size_t ch)
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

template <std::size_t Ch> void dma_l1l4::control_written(std::uint32_t old, std::uint32_t now)
{
  auto const& c = ch_[Ch];
  if(c.enable.of(now) != 0 and c.enable.of(old) == 0)
  {
    start(Ch);
  }
  interrupts(Ch);
  schedule(ctx_.clock.time().ps);
}

template <std::size_t Ch> void dma_l1l4::count_written(std::uint32_t old, std::uint32_t)
{
  if(get(regs_, ch_[Ch].enable) != 0)
  {
    regs_.poke(ch_[Ch].cndtr.offset, old);
  }
}

void dma_l1l4::clear_written(std::uint32_t, std::uint32_t now)
{
  for(std::size_t ch = 0; ch < variant_.channels; ++ch)
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

void dma_l1l4::selection_written(std::uint32_t, std::uint32_t)
{
  schedule(ctx_.clock.time().ps);
}

auto dma_l1l4::on_write(
    register_file& regs,
    std::uint32_t  offset,
    std::uint8_t   size,
    std::uint32_t  old,
    std::uint32_t  value,
    access_ctx     ctx) -> std::expected<void, bus_error>
{
  if(variant_.word_only and size != 4 and ctx.by != initiator::debugger)
  {
    auto const word  = offset & ~3u;
    auto const named = std::ranges::find_if(
        desc_.registers, [&](register_desc const& r) { return r.offset == word; });
    auto const name = named != desc_.registers.end() ? named->name : std::string{};
    warn_once_or_refuse(narrow_logged_, name, word, [&] {
      return std::format(
          "a {}-byte write to {}: the DMA registers take 32-bit accesses, RM0351 11.6", size, name);
    });
  }
  return dispatch_model::on_write(regs, offset, size, old, value, ctx);
}

auto dma_l1l4::request_of(std::string_view signal) const -> std::optional<std::uint32_t>
{
  auto const found = std::ranges::find(variant_.requests, signal, &dma_l1l4_request::signal);
  if(found == variant_.requests.end())
  {
    return std::nullopt;
  }
  return static_cast<std::uint32_t>(found - variant_.requests.begin());
}

void dma_l1l4::on_request(std::uint32_t request, bool level)
{
  if(request >= max_requests or request >= variant_.requests.size())
  {
    return;
  }
  auto const signal = variant_.requests[request].signal;
  for(std::size_t r = 0; r < variant_.requests.size(); ++r)
  {
    if(variant_.requests[r].signal == signal)
    {
      state_.request[r] = level ? 1 : 0;
    }
  }
  if(level)
  {
    schedule(ctx_.clock.time().ps);
  }
}
}

auto dma_l1l4_factory(dma_l1l4_variant variant) -> model_factory
{
  return [variant](model_context& ctx, peripheral_desc const& desc) -> std::shared_ptr<model> {
    auto const base = writes_by_count(variant.channels, std::make_index_sequence<max_channels>{});
    std::vector<write_entry<dma_l1l4>> table{base.begin(), base.end()};
    if(not variant.selection_register.empty())
    {
      table.push_back({variant.selection_register, &dma_l1l4::selection_written});
    }
    return std::make_shared<dma_l1l4>(ctx, desc, variant, table);
  };
}

namespace
{
constexpr auto dma1_instances = std::to_array<std::string_view>({"DMA1"});
constexpr auto dma2_instances = std::to_array<std::string_view>({"DMA2"});
constexpr auto dma_l1l4_table = std::to_array<builtin>({
    row_of<^^dma_l1l4_factory, ^^l1_dma1>(
        {.ip              = "DMA",
         .ip_version_glob = "STM32L1*_dma_*",
         .instance_globs  = dma1_instances,
         .families        = family::l1}),
    row_of<^^dma_l1l4_factory, ^^l1_dma2>(
        {.ip              = "DMA",
         .ip_version_glob = "STM32L1*_dma_*",
         .instance_globs  = dma2_instances,
         .families        = family::l1}),
    row_of<^^dma_l1l4_factory, ^^l4_dma1>(
        {.ip              = "DMA",
         .ip_version_glob = "STM32L4*_dma_*",
         .instance_globs  = dma1_instances,
         .families        = family::l4}),
    row_of<^^dma_l1l4_factory, ^^l4_dma2>(
        {.ip              = "DMA",
         .ip_version_glob = "STM32L4*_dma_*",
         .instance_globs  = dma2_instances,
         .families        = family::l4}),
});
}

auto dma_l1l4_rows() -> std::span<builtin const>
{
  return dma_l1l4_table;
}
}
