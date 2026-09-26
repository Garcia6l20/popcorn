#include "dma.hpp"
#include "factories.hpp"
#include "gpdma_requests.hpp"
#include "model_base.hpp"

#include <popcorn/core/clock.hpp>
#include <popcorn/core/services.hpp>

#include <algorithm>
#include <array>
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
constexpr std::size_t   channels      = 16;
constexpr std::size_t   first_2d      = 12;
constexpr std::size_t   requests      = 128;
constexpr std::uint32_t burst_done    = 0;
constexpr std::uint64_t ps_per_second = 1'000'000'000'000;

struct channel_state
{
  std::uint32_t generation{};
  std::uint32_t block_bytes{};
  bool          half_reported{};
  bool          scheduled{};

  friend auto operator==(channel_state const&, channel_state const&) -> bool = default;
};

struct gpdma_state
{
  std::array<channel_state, channels> channel{};
  std::array<std::uint8_t, requests>  request{};

  friend auto operator==(gpdma_state const&, gpdma_state const&) -> bool = default;
};

struct channel_fields
{
  reg_ref                  lbar;
  reg_ref                  fcr;
  reg_ref                  sr;
  reg_ref                  cr;
  reg_ref                  tr1;
  reg_ref                  tr2;
  reg_ref                  br1;
  reg_ref                  sar;
  reg_ref                  dar;
  reg_ref                  llr;
  field_ref                enable;
  field_ref                reset;
  field_ref                suspend;
  field_ref                link_step;
  field_ref                idle;
  field_ref                suspended;
  field_ref                source_width;
  field_ref                source_increment;
  field_ref                source_burst;
  field_ref                destination_width;
  field_ref                destination_increment;
  field_ref                request;
  field_ref                software;
  field_ref                event_mode;
  field_ref                bytes;
  field_ref                link_base;
  field_ref                link_address;
  field_ref                update_link;
  field_ref                update_destination;
  field_ref                update_source;
  field_ref                update_block;
  field_ref                update_tr2;
  field_ref                update_tr1;
  std::array<field_ref, 7> flag;
  std::array<field_ref, 7> clear;
  std::array<field_ref, 7> interrupt_enable;
  field_ref                packing;
  std::optional<reg_ref>   tr3;
  std::optional<reg_ref>   br2;
  std::optional<field_ref> update_tr3;
  std::optional<field_ref> update_br2;
  irq_line*                line{};
};

constexpr std::array<std::string_view, 7> flag_names{"TCF",  "HTF",   "DTEF", "ULEF",
                                                     "USEF", "SUSPF", "TOF"};
constexpr std::array<std::string_view, 7> enable_names{"TCIE",  "HTIE",   "DTEIE", "ULEIE",
                                                       "USEIE", "SUSPIE", "TOIE"};

enum flag_index : std::size_t
{
  transfer_complete,
  half_transfer,
  data_error,
  link_error,
  user_error,
  suspended_flag,
};

enum class unsupported : std::uint8_t
{
  two_d,
  link_step,
  packing,
};

class gpdma;

auto gpdma_writes() -> std::span<write_entry<gpdma> const>;

/**
 * @brief GPDMA1 of the STM32U5, RM0456 section 17: 16 channels, software or hardware requested,
 *        1D transfers in bursts, block and linked-list transfers, half and complete transfer,
 *        error, suspend and reset. Each burst is an event a transfer latency after its request,
 *        never performed inside a register or request callback. Not modelled: 2D addressing on
 *        channels 12 to 15, whose TR3 and BR2 linked-list words are still consumed in RM order,
 *        the FIFO packing modes of PAM, link step mode, each logging once and running as plain 1D
 *        run-to-completion, triggers, and the half transfer event under TCEM 2 and 3, which is
 *        only raised per block under TCEM 0 and 1. A request level held by a requester across a
 *        machine reset is cleared: the requester announces it again on its next change.
 */
class gpdma final : public dispatch_model<gpdma, gpdma_state>, public dma_controller
{
public:
  gpdma(model_context& ctx, peripheral_desc const& desc);

  void               reset(register_file& regs, reset_kind kind) override;
  void               on_event(std::uint32_t kind, std::uint64_t payload) override;
  void               on_request(std::uint32_t request, bool level) override;
  [[nodiscard]] auto request_of(std::string_view signal) const
      -> std::optional<std::uint32_t> override;

  template <std::size_t Ch> void control_written(std::uint32_t old, std::uint32_t now);
  template <std::size_t Ch> void clear_written(std::uint32_t old, std::uint32_t now);
  void                           plain_written(std::uint32_t old, std::uint32_t now);

private:
  [[nodiscard]] auto active(std::size_t ch) const -> bool;
  [[nodiscard]] auto wants_burst(std::size_t ch) const -> bool;
  [[nodiscard]] auto valid(std::size_t ch) const -> bool;
  void               start(std::size_t ch);
  void               schedule(std::size_t ch);
  void               burst(std::size_t ch);
  void               block_done(std::size_t ch);
  auto               link(std::size_t ch) -> bool;
  void               finish(std::size_t ch, flag_index flag);
  void               raise_flag(std::size_t ch, flag_index flag);
  void               interrupts(std::size_t ch);
  void               warn_once(std::size_t ch, unsupported feature);

  register_file&                       regs_;
  std::array<channel_fields, channels> ch_{};
  reg_ref                              misr_;
  std::array<field_ref, channels>      masked_{};
  core::clock_domain                   hclk_;
  std::array<bool, 3>                  unsupported_logged_{};
  bool                                 stopped_clock_logged_{};
};

template <std::size_t... Ch>
auto channel_entries(std::index_sequence<Ch...>, std::deque<std::string>& names)
    -> std::vector<write_entry<gpdma>>
{
  std::vector<write_entry<gpdma>> out;
  auto const add = [&](std::string name, void (gpdma::*handler)(std::uint32_t, std::uint32_t)) {
    names.push_back(std::move(name));
    out.push_back({names.back(), handler});
  };
  (
      [&] {
        auto const prefix = std::format("GPDMA_C{}", Ch);
        add(prefix + "LBAR", &gpdma::plain_written);
        add(prefix + "FCR", &gpdma::clear_written<Ch>);
        add(prefix + "SR", &gpdma::plain_written);
        add(prefix + "CR", &gpdma::control_written<Ch>);
        add(prefix + "TR1", &gpdma::plain_written);
        add(prefix + "TR2", &gpdma::plain_written);
        add(prefix + "BR1", &gpdma::plain_written);
        add(prefix + "SAR", &gpdma::plain_written);
        add(prefix + "DAR", &gpdma::plain_written);
        add(prefix + "LLR", &gpdma::plain_written);
        if(Ch >= first_2d)
        {
          add(prefix + "TR3", &gpdma::plain_written);
          add(prefix + "BR2", &gpdma::plain_written);
        }
      }(),
      ...);
  names.push_back("GPDMA_MISR");
  out.push_back({names.back(), &gpdma::plain_written});
  return out;
}

auto gpdma_writes() -> std::span<write_entry<gpdma> const>
{
  static std::deque<std::string>               names;
  static std::vector<write_entry<gpdma>> const table =
      channel_entries(std::make_index_sequence<channels>{}, names);
  return table;
}

gpdma::gpdma(model_context& ctx, peripheral_desc const& desc)
    : dispatch_model{ctx, desc, "GPDMA", gpdma_writes()}, regs_{ctx.regs}, misr_{reg("GPDMA_MISR")},
      hclk_{ctx.clock.domain("hclk")}
{
  for(std::size_t ch = 0; ch < channels; ++ch)
  {
    auto const p            = std::format("GPDMA_C{}", ch);
    auto&      c            = ch_[ch];
    c.lbar                  = reg(p + "LBAR");
    c.fcr                   = reg(p + "FCR");
    c.sr                    = reg(p + "SR");
    c.cr                    = reg(p + "CR");
    c.tr1                   = reg(p + "TR1");
    c.tr2                   = reg(p + "TR2");
    c.br1                   = reg(p + "BR1");
    c.sar                   = reg(p + "SAR");
    c.dar                   = reg(p + "DAR");
    c.llr                   = reg(p + "LLR");
    c.enable                = field(p + "CR", "EN");
    c.reset                 = field(p + "CR", "RESET");
    c.suspend               = field(p + "CR", "SUSP");
    c.link_step             = field(p + "CR", "LSM");
    c.idle                  = field(p + "SR", "IDLEF");
    c.suspended             = field(p + "SR", "SUSPF");
    c.source_width          = field(p + "TR1", "SDW_LOG2");
    c.source_increment      = field(p + "TR1", "SINC");
    c.source_burst          = field(p + "TR1", "SBL_1");
    c.destination_width     = field(p + "TR1", "DDW_LOG2");
    c.destination_increment = field(p + "TR1", "DINC");
    c.request               = field(p + "TR2", "REQSEL");
    c.software              = field(p + "TR2", "SWREQ");
    c.event_mode            = field(p + "TR2", "TCEM");
    c.bytes                 = field(p + "BR1", "BNDT");
    c.link_base             = field(p + "LBAR", "LBA");
    c.link_address          = field(p + "LLR", "LA");
    c.update_link           = field(p + "LLR", "ULL");
    c.update_destination    = field(p + "LLR", "UDA");
    c.update_source         = field(p + "LLR", "USA");
    c.update_block          = field(p + "LLR", "UB1");
    c.update_tr2            = field(p + "LLR", "UT2");
    c.update_tr1            = field(p + "LLR", "UT1");
    c.packing               = field(p + "TR1", "PAM");
    for(std::size_t f = 0; f < flag_names.size(); ++f)
    {
      c.flag[f]             = field(p + "SR", flag_names[f]);
      c.clear[f]            = field(p + "FCR", flag_names[f]);
      c.interrupt_enable[f] = field(p + "CR", enable_names[f]);
    }
    if(ch >= first_2d)
    {
      c.tr3        = reg(p + "TR3");
      c.br2        = reg(p + "BR2");
      c.update_tr3 = field(p + "LLR", "UT3");
      c.update_br2 = field(p + "LLR", "UB2");
    }
    c.line      = &irq(std::format("GPDMA1_Channel{}_IRQn", ch));
    masked_[ch] = field("GPDMA_MISR", std::format("MIS{}", ch));
  }
}

void gpdma::reset(register_file& regs, reset_kind)
{
  for(auto const& name : claims())
  {
    regs.poke(reg(name).offset, describe(name).reset);
  }
  state_.request = {};
  for(auto& c : state_.channel)
  {
    ++c.generation;
    c.scheduled     = false;
    c.half_reported = false;
    c.block_bytes   = 0;
  }
  for(std::size_t ch = 0; ch < channels; ++ch)
  {
    interrupts(ch);
  }
}

void gpdma::plain_written(std::uint32_t, std::uint32_t)
{}

auto gpdma::active(std::size_t ch) const -> bool
{
  auto const& c = ch_[ch];
  return get(regs_, c.enable) != 0 and get(regs_, c.suspend) == 0;
}

auto gpdma::wants_burst(std::size_t ch) const -> bool
{
  auto const& c = ch_[ch];
  if(not active(ch) or get(regs_, c.bytes) == 0)
  {
    return false;
  }
  return get(regs_, c.software) != 0 or state_.request[get(regs_, c.request)] != 0;
}

auto gpdma::valid(std::size_t ch) const -> bool
{
  auto const& c     = ch_[ch];
  auto const  bytes = get(regs_, c.bytes);
  auto const  sw    = std::uint32_t{1} << get(regs_, c.source_width);
  auto const  dw    = std::uint32_t{1} << get(regs_, c.destination_width);
  if(bytes == 0 and get(regs_, c.update_block) == 0)
  {
    return false;
  }
  return get(regs_, c.source_width)
       < 3
     and get(regs_, c.destination_width)
       < 3
     and bytes
       % sw
      == 0
     and regs_.peek(c.sar.offset)
       % sw
      == 0
     and regs_.peek(c.dar.offset)
       % dw
      == 0;
}

void gpdma::warn_once(std::size_t ch, unsupported feature)
{
  constexpr std::array<std::string_view, 3> names{
      "2D addressing", "link step mode", "data packing"};
  auto const what = names[std::to_underlying(feature)];
  if(not std::exchange(unsupported_logged_[std::to_underlying(feature)], true))
  {
    warn(
        std::format("GPDMA_C{}CR", ch),
        std::format("channel {} uses {}, not modelled: it runs as a 1D transfer", ch, what));
  }
}

void gpdma::start(std::size_t ch)
{
  auto& c = ch_[ch];
  auto& s = state_.channel[ch];
  if(not valid(ch))
  {
    finish(ch, user_error);
    return;
  }
  if(get(regs_, c.link_step) != 0)
  {
    warn_once(ch, unsupported::link_step);
  }
  if(c.br2 and (regs_.peek(c.br2->offset) != 0 or regs_.peek(c.tr3->offset) != 0))
  {
    warn_once(ch, unsupported::two_d);
  }
  if(get(regs_, c.source_width)
     != get(regs_, c.destination_width)
     and (get(regs_, c.packing) & 2u)
     != 0)
  {
    warn_once(ch, unsupported::packing);
  }
  update(regs_, c.idle, 0);
  if(get(regs_, c.bytes) == 0 and not link(ch))
  {
    return;
  }
  s.block_bytes   = get(regs_, c.bytes);
  s.half_reported = false;
  schedule(ch);
}

void gpdma::schedule(std::size_t ch)
{
  auto& s = state_.channel[ch];
  if(s.scheduled or not wants_burst(ch))
  {
    return;
  }
  auto const hz = ctx_.clock.hz(hclk_);
  if(hz == 0)
  {
    if(not std::exchange(stopped_clock_logged_, true))
    {
      warn(std::format("GPDMA_C{}CR", ch), "HCLK is stopped: the transfer waits");
    }
    return;
  }
  constexpr std::uint64_t latency_cycles = 4;
  auto const              ps             = latency_cycles * ps_per_second / hz;
  s.scheduled                            = true;
  ctx_.events.schedule(
      ctx_.clock.to_icount(vtime{ctx_.clock.time().ps + ps}), burst_done,
      (static_cast<std::uint64_t>(ch) << 32) | s.generation);
}

void gpdma::on_event(std::uint32_t kind, std::uint64_t payload)
{
  auto const ch = static_cast<std::size_t>(payload >> 32);
  if(kind
     != burst_done
     or ch
     >= channels
     or static_cast<std::uint32_t>(payload)
     != state_.channel[ch].generation)
  {
    return;
  }
  state_.channel[ch].scheduled = false;
  if(wants_burst(ch))
  {
    burst(ch);
  }
}

void gpdma::burst(std::size_t ch)
{
  auto&      c     = ch_[ch];
  auto&      s     = state_.channel[ch];
  auto const sw    = static_cast<std::uint8_t>(1u << get(regs_, c.source_width));
  auto const dw    = static_cast<std::uint8_t>(1u << get(regs_, c.destination_width));
  auto const beats = get(regs_, c.source_burst) + 1;
  for(std::uint32_t beat = 0; beat < beats and get(regs_, c.bytes) != 0; ++beat)
  {
    auto const source      = regs_.peek(c.sar.offset);
    auto const destination = regs_.peek(c.dar.offset);
    auto const value       = ctx_.services.read(source, sw, initiator::dma);
    if(not value)
    {
      finish(ch, data_error);
      return;
    }
    auto const mask   = dw >= 4 ? ~std::uint32_t{} : (std::uint32_t{1} << (8u * dw)) - 1u;
    auto const stored = ctx_.services.write(destination, dw, *value & mask, initiator::dma);
    if(not stored)
    {
      finish(ch, data_error);
      return;
    }
    if(get(regs_, c.source_increment) != 0)
    {
      regs_.poke(c.sar.offset, source + sw);
    }
    if(get(regs_, c.destination_increment) != 0)
    {
      regs_.poke(c.dar.offset, destination + dw);
    }
    update(regs_, c.bytes, get(regs_, c.bytes) - sw);
  }
  auto const mode = get(regs_, c.event_mode);
  if(mode < 2 and not s.half_reported and get(regs_, c.bytes) * 2 <= s.block_bytes)
  {
    s.half_reported = true;
    raise_flag(ch, half_transfer);
  }
  if(get(regs_, c.bytes) == 0)
  {
    block_done(ch);
    return;
  }
  schedule(ch);
}

void gpdma::block_done(std::size_t ch)
{
  auto&      c    = ch_[ch];
  auto&      s    = state_.channel[ch];
  auto const mode = get(regs_, c.event_mode);
  if(regs_.peek(c.llr.offset) == 0)
  {
    finish(ch, transfer_complete);
    return;
  }
  if(mode < 2)
  {
    raise_flag(ch, transfer_complete);
  }
  if(not link(ch))
  {
    return;
  }
  if(mode == 2)
  {
    raise_flag(ch, transfer_complete);
  }
  s.block_bytes   = get(regs_, c.bytes);
  s.half_reported = false;
  schedule(ch);
}

auto gpdma::link(std::size_t ch) -> bool
{
  auto&      c    = ch_[ch];
  auto const llr  = regs_.peek(c.llr.offset);
  auto const base = get(regs_, c.link_base) << 16;
  auto       at   = static_cast<popcorn::address>(base | c.link_address.of(llr) << 2);
  auto const load = [&](field_ref const& update_bit, reg_ref const& target) {
    if(update_bit.of(llr) == 0)
    {
      return true;
    }
    auto const word = ctx_.services.read(at, 4, initiator::dma);
    if(not word)
    {
      return false;
    }
    regs_.poke(target.offset, *word);
    at += 4;
    return true;
  };
  auto const two_d = [&] {
    return not c.tr3 or (load(*c.update_tr3, *c.tr3) and load(*c.update_br2, *c.br2));
  };
  auto const loaded = load(c.update_tr1, c.tr1)
                  and load(c.update_tr2, c.tr2)
                  and load(c.update_block, c.br1)
                  and load(c.update_source, c.sar)
                  and load(c.update_destination, c.dar)
                  and two_d()
                  and load(c.update_link, c.llr);
  if(not loaded)
  {
    finish(ch, link_error);
    return false;
  }
  if(c.update_link.of(llr) == 0)
  {
    regs_.poke(c.llr.offset, 0);
  }
  if(not valid(ch))
  {
    finish(ch, user_error);
    return false;
  }
  return true;
}

void gpdma::finish(std::size_t ch, flag_index flag)
{
  auto& c = ch_[ch];
  auto& s = state_.channel[ch];
  ++s.generation;
  s.scheduled = false;
  update(regs_, c.enable, 0);
  update(regs_, c.idle, 1);
  raise_flag(ch, flag);
}

void gpdma::raise_flag(std::size_t ch, flag_index flag)
{
  update(regs_, ch_[ch].flag[flag], 1);
  interrupts(ch);
}

void gpdma::interrupts(std::size_t ch)
{
  auto const& c     = ch_[ch];
  bool        level = false;
  for(std::size_t f = 0; f < c.flag.size(); ++f)
  {
    level = level or (get(regs_, c.flag[f]) != 0 and get(regs_, c.interrupt_enable[f]) != 0);
  }
  update(regs_, masked_[ch], level ? 1 : 0);
  if(level)
  {
    c.line->raise();
  }
  else
  {
    c.line->lower();
  }
}

template <std::size_t Ch> void gpdma::control_written(std::uint32_t old, std::uint32_t now)
{
  auto& c = ch_[Ch];
  auto& s = state_.channel[Ch];
  if(c.reset.of(now) != 0)
  {
    ++s.generation;
    s.scheduled = false;
    now         = c.reset.with(now, 0);
    now         = c.enable.with(now, 0);
    now         = c.suspend.with(now, 0);
    regs_.poke(c.cr.offset, now);
    update(regs_, c.idle, 1);
    update(regs_, c.suspended, 0);
    interrupts(Ch);
    return;
  }
  auto const was_on        = c.enable.of(old) != 0;
  auto const is_on         = c.enable.of(now) != 0;
  auto const was_suspended = c.suspend.of(old) != 0;
  auto const is_suspended  = c.suspend.of(now) != 0;
  if(is_suspended and not was_suspended and was_on)
  {
    ++s.generation;
    s.scheduled = false;
    update(regs_, c.idle, 1);
    raise_flag(Ch, suspended_flag);
  }
  else if(was_suspended and not is_suspended and is_on)
  {
    update(regs_, c.idle, 0);
    schedule(Ch);
  }
  if(is_on and not was_on)
  {
    start(Ch);
  }
  interrupts(Ch);
}

template <std::size_t Ch> void gpdma::clear_written(std::uint32_t, std::uint32_t now)
{
  auto& c = ch_[Ch];
  for(std::size_t f = 0; f < c.flag.size(); ++f)
  {
    if(c.clear[f].of(now) != 0)
    {
      update(regs_, c.flag[f], 0);
    }
  }
  regs_.poke(c.fcr.offset, 0);
  interrupts(Ch);
}

auto gpdma::request_of(std::string_view signal) const -> std::optional<std::uint32_t>
{
  auto const found = std::ranges::find(gpdma_requests, signal, &gpdma_request::signal);
  if(found == gpdma_requests.end())
  {
    return std::nullopt;
  }
  return found->request;
}

void gpdma::on_request(std::uint32_t request, bool level)
{
  if(request >= requests)
  {
    return;
  }
  state_.request[request] = level ? 1 : 0;
  if(not level)
  {
    return;
  }
  for(std::size_t ch = 0; ch < channels; ++ch)
  {
    if(get(regs_, ch_[ch].software) == 0 and get(regs_, ch_[ch].request) == request)
    {
      schedule(ch);
    }
  }
}
}

auto gpdma_factory() -> model_factory
{
  return [](model_context& ctx, peripheral_desc const& desc) -> std::shared_ptr<model> {
    return std::make_shared<gpdma>(ctx, desc);
  };
}
}
