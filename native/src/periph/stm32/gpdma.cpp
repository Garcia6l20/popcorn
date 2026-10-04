#include "dma.hpp"
#include "dma_data.hpp"
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
  std::uint16_t repeat{};
  std::uint32_t packed{};
  std::uint8_t  packed_bytes{};
  bool          half_reported{};
  bool          scheduled{};
  std::uint8_t  destination_beats{};

  friend auto operator==(channel_state const&, channel_state const&) -> bool = default;
};

struct gpdma_state
{
  std::array<channel_state, channels> channel{};
  std::array<std::uint8_t, requests>  request{};

  friend auto operator==(gpdma_state const&, gpdma_state const&) -> bool = default;
};

/** @brief The 2D addressing fields of channels 12 to 15 and their LLI updates, RM0456 17.8. */
struct two_d_fields
{
  reg_ref   tr3;
  reg_ref   br2;
  field_ref update_tr3;
  field_ref update_br2;
  field_ref repeat;
  field_ref source_decrement;
  field_ref destination_decrement;
  field_ref block_source_decrement;
  field_ref block_destination_decrement;
  field_ref source_offset;
  field_ref destination_offset;
  field_ref block_source_offset;
  field_ref block_destination_offset;
  field_ref destination_burst;
};

struct channel_fields
{
  reg_ref                     lbar;
  reg_ref                     fcr;
  reg_ref                     sr;
  reg_ref                     cr;
  reg_ref                     tr1;
  reg_ref                     tr2;
  reg_ref                     br1;
  reg_ref                     sar;
  reg_ref                     dar;
  reg_ref                     llr;
  field_ref                   enable;
  field_ref                   reset;
  field_ref                   suspend;
  field_ref                   link_step;
  field_ref                   idle;
  field_ref                   suspended;
  field_ref                   source_width;
  field_ref                   source_increment;
  field_ref                   source_burst;
  field_ref                   destination_width;
  field_ref                   destination_increment;
  field_ref                   request;
  field_ref                   software;
  field_ref                   event_mode;
  field_ref                   bytes;
  field_ref                   link_base;
  field_ref                   link_address;
  field_ref                   update_link;
  field_ref                   update_destination;
  field_ref                   update_source;
  field_ref                   update_block;
  field_ref                   update_tr2;
  field_ref                   update_tr1;
  std::array<field_ref, 7>    flag;
  std::array<field_ref, 7>    clear;
  std::array<field_ref, 7>    interrupt_enable;
  field_ref                   packing;
  field_ref                   source_exchange;
  field_ref                   byte_exchange;
  field_ref                   half_exchange;
  std::optional<two_d_fields> two_d;
  irq_line*                   line{};
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
  link_step,
};

class gpdma;

auto gpdma_writes() -> std::span<write_entry<gpdma> const>;

/**
 * @brief GPDMA1 of the STM32U5, RM0456 section 17: 16 channels, software or hardware requested,
 *        1D transfers in bursts, block and linked-list transfers, half and complete transfer,
 *        error, suspend and reset. Each burst is an event a transfer latency after its request,
 *        never performed inside a register or request callback. Data handling, RM0456 17.4.10
 *        and Table 141: SBX, then the width conversion of PAM, 0-padding or sign extension to a
 *        wider destination, right or left truncation to a narrower one, packing and unpacking
 *        LSB first, then DBX and DHX. BNDT counts source bytes. A packing channel needs BNDT to
 *        be a multiple of the destination width. A link with LLR.UB1 = 0 restores BNDT and BRC
 *        to the values the item started with, RM0456 GPDMA_CxBR1, so a last item with every update
 *        bit 0 and LA not 0 runs again without reading memory. A link with LLR.ULL = 0 keeps LLR:
 *        the item is recursive and the next link reads it again. Only LLR = 0 completes the
 *        channel. Channels 12 to 15 add 2D addressing, RM0456 17.4.10: BRC + 1 blocks per
 *        item, BNDT reloaded for each. SAO is added, or subtracted under SDEC, after each source
 *        burst of SBL_1 + 1 beats, never after a burst shortened by the block end. DAO likewise
 *        after each destination burst of DBL_1 + 1 items, counted after the data handling, so
 *        packing and unpacking change the count. A destination burst left partial by the block
 *        end gets no DAO, an assumption: RM0456 is silent on it. BRSAO and BRDAO apply after
 *        every block, per BRSDEC and BRDDEC, on top of the burst offsets. SAO, BRSAO not a
 *        multiple of the source width and DAO, BRDAO not a multiple of the destination width
 *        are user setting errors. A state saved before the destination burst count existed loads
 *        with the count 0, exact when the save fell on a destination burst boundary, as it always
 *        does with one-item destination bursts. Not modelled: link step mode, logging once and
 *        running to completion, and triggers. The half transfer event rises at the half of each
 *        block under TCEM 0, 1 and 2, and only in the last item, the one running with LLR = 0,
 *        under TCEM 3. On channels 12 to 15, TCEM 0 keeps the block level, while TCEM 1 and 2,
 *        and 3 in the last item, raise the half transfer once the rounded-up half of the BRC + 1
 *        blocks is done, and TCEM 1 raises the complete transfer at the end of the 2D block.
 *        With BRC 0 that half is the one block, so the half transfer rises at the block end,
 *        RM0456 GPDMA_CxTR2.TCEM and 17.7. A channel enabled with BNDT 0 raises the complete
 *        transfer event once its first item is loaded when its TCEM before that load is 2, and
 *        no event under 0 and 1. A request level held by a requester across a machine reset is
 *        cleared: the requester announces it again on its next change.
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
  auto               convert(std::size_t ch, std::uint32_t value) -> bool;
  auto               deliver(std::size_t ch, std::uint32_t value) -> bool;
  void               block_done(std::size_t ch);
  void               offset(field_ref amount, field_ref decrement, reg_ref target);
  void               block_offsets(std::size_t ch);
  void               two_d_half(std::size_t ch, std::uint32_t mode);
  auto               repeat_block(std::size_t ch, std::uint32_t mode) -> bool;
  auto               link(std::size_t ch) -> bool;
  void               latch(std::size_t ch);
  void               finish(std::size_t ch, flag_index flag);
  void               raise_flag(std::size_t ch, flag_index flag);
  void               interrupts(std::size_t ch);
  void               warn_once(std::size_t ch, unsupported feature);

  register_file&                       regs_;
  std::array<channel_fields, channels> ch_{};
  reg_ref                              misr_;
  std::array<field_ref, channels>      masked_{};
  core::clock_domain                   hclk_;
  std::array<bool, 1>                  unsupported_logged_{};
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
    c.source_exchange       = field(p + "TR1", "SBX");
    c.byte_exchange         = field(p + "TR1", "DBX");
    c.half_exchange         = field(p + "TR1", "DHX");
    for(std::size_t f = 0; f < flag_names.size(); ++f)
    {
      c.flag[f]             = field(p + "SR", flag_names[f]);
      c.clear[f]            = field(p + "FCR", flag_names[f]);
      c.interrupt_enable[f] = field(p + "CR", enable_names[f]);
    }
    if(ch >= first_2d)
    {
      c.two_d = two_d_fields{
          .tr3                         = reg(p + "TR3"),
          .br2                         = reg(p + "BR2"),
          .update_tr3                  = field(p + "LLR", "UT3"),
          .update_br2                  = field(p + "LLR", "UB2"),
          .repeat                      = field(p + "BR1", "BRC"),
          .source_decrement            = field(p + "BR1", "SDEC"),
          .destination_decrement       = field(p + "BR1", "DDEC"),
          .block_source_decrement      = field(p + "BR1", "BRSDEC"),
          .block_destination_decrement = field(p + "BR1", "BRDDEC"),
          .source_offset               = field(p + "TR3", "SAO"),
          .destination_offset          = field(p + "TR3", "DAO"),
          .block_source_offset         = field(p + "BR2", "BRSAO"),
          .block_destination_offset    = field(p + "BR2", "BRDAO"),
          .destination_burst           = field(p + "TR1", "DBL_1"),
      };
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
    c.scheduled         = false;
    c.half_reported     = false;
    c.block_bytes       = 0;
    c.repeat            = 0;
    c.packed            = 0;
    c.packed_bytes      = 0;
    c.destination_beats = 0;
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
  auto const widths  = get(regs_, c.source_width) < 3 and get(regs_, c.destination_width) < 3;
  auto const packing = sw != dw and (get(regs_, c.packing) & 2u) != 0;
  auto const whole   = bytes % sw == 0 and (not packing or bytes % dw == 0);
  auto const source  = regs_.peek(c.sar.offset) % sw == 0;
  auto const target  = regs_.peek(c.dar.offset) % dw == 0;
  auto const offsets = not c.two_d
                    or (get(regs_, c.two_d->source_offset)
                        % sw
                        == 0
                        and get(regs_, c.two_d->block_source_offset)
                        % sw
                        == 0
                        and get(regs_, c.two_d->destination_offset)
                        % dw
                        == 0
                        and get(regs_, c.two_d->block_destination_offset)
                        % dw
                        == 0);
  return widths and whole and source and target and offsets;
}

void gpdma::warn_once(std::size_t ch, unsupported feature)
{
  constexpr std::array<std::string_view, 1> names{"link step mode"};
  auto const                                what = names[std::to_underlying(feature)];
  if(not std::exchange(unsupported_logged_[std::to_underlying(feature)], true))
  {
    warn(
        std::format("GPDMA_C{}CR", ch),
        std::format("channel {} uses {}, not modelled: it runs to completion", ch, what));
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
  update(regs_, c.idle, 0);
  if(get(regs_, c.bytes) == 0)
  {
    auto const mode = get(regs_, c.event_mode);
    if(not link(ch))
    {
      return;
    }
    if(mode == 2)
    {
      raise_flag(ch, transfer_complete);
    }
  }
  latch(ch);
  s.half_reported     = false;
  s.packed            = 0;
  s.packed_bytes      = 0;
  s.destination_beats = 0;
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
  ctx_.events.schedule_at(
      vtime{ctx_.clock.time().ps + ps}, burst_done,
      (static_cast<std::uint64_t>(ch) << 32) | s.generation, ctx_.clock);
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

auto gpdma::deliver(std::size_t ch, std::uint32_t value) -> bool
{
  auto&      c           = ch_[ch];
  auto const dw          = static_cast<std::uint8_t>(1u << get(regs_, c.destination_width));
  auto const destination = regs_.peek(c.dar.offset);
  auto const bytes       = get(regs_, c.byte_exchange) != 0;
  auto const halves      = get(regs_, c.half_exchange) != 0;
  auto const data        = exchange_destination(value, dw, bytes, halves);
  if(not ctx_.services.write(destination, dw, data, initiator::dma))
  {
    return false;
  }
  if(get(regs_, c.destination_increment) != 0)
  {
    regs_.poke(c.dar.offset, destination + dw);
  }
  if(c.two_d)
  {
    auto& s = state_.channel[ch];
    if(++s.destination_beats > get(regs_, c.two_d->destination_burst))
    {
      s.destination_beats = 0;
      offset(c.two_d->destination_offset, c.two_d->destination_decrement, c.dar);
    }
  }
  return true;
}

auto gpdma::convert(std::size_t ch, std::uint32_t value) -> bool
{
  auto&      c      = ch_[ch];
  auto&      s      = state_.channel[ch];
  auto const sw     = static_cast<std::uint8_t>(1u << get(regs_, c.source_width));
  auto const dw     = static_cast<std::uint8_t>(1u << get(regs_, c.destination_width));
  auto const pam    = get(regs_, c.packing);
  auto const source = exchange_source(value, sw, get(regs_, c.source_exchange) != 0);
  if(sw == dw or (pam & 2u) == 0)
  {
    return deliver(ch, resize(source, sw, dw, pam));
  }
  if(dw < sw)
  {
    for(std::uint32_t at = 0; at < sw; at += dw)
    {
      if(not deliver(ch, (source >> (8u * at)) & data_mask(dw)))
      {
        return false;
      }
    }
    return true;
  }
  s.packed |= (source & data_mask(sw)) << (8u * s.packed_bytes);
  s.packed_bytes = static_cast<std::uint8_t>(s.packed_bytes + sw);
  if(s.packed_bytes < dw)
  {
    return true;
  }
  auto const packed = std::exchange(s.packed, 0);
  s.packed_bytes    = 0;
  return deliver(ch, packed);
}

void gpdma::burst(std::size_t ch)
{
  auto&         c     = ch_[ch];
  auto&         s     = state_.channel[ch];
  auto const    sw    = static_cast<std::uint8_t>(1u << get(regs_, c.source_width));
  auto const    beats = get(regs_, c.source_burst) + 1;
  std::uint32_t beat  = 0;
  for(; beat < beats and get(regs_, c.bytes) != 0; ++beat)
  {
    auto const source = regs_.peek(c.sar.offset);
    auto const value  = ctx_.services.read(source, sw, initiator::dma);
    if(not value or not convert(ch, *value))
    {
      finish(ch, data_error);
      return;
    }
    if(get(regs_, c.source_increment) != 0)
    {
      regs_.poke(c.sar.offset, source + sw);
    }
    update(regs_, c.bytes, get(regs_, c.bytes) - sw);
  }
  if(c.two_d and beat == beats)
  {
    offset(c.two_d->source_offset, c.two_d->source_decrement, c.sar);
  }
  auto const mode = get(regs_, c.event_mode);
  auto const last = regs_.peek(c.llr.offset) == 0;
  auto const half = c.two_d ? mode == 0 : mode < 3 or last;
  if(half and not s.half_reported and get(regs_, c.bytes) * 2 <= s.block_bytes)
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
  if(c.two_d and repeat_block(ch, mode))
  {
    return;
  }
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
  latch(ch);
  s.half_reported = false;
  schedule(ch);
}

auto gpdma::repeat_block(std::size_t ch, std::uint32_t mode) -> bool
{
  auto const& c = ch_[ch];
  auto&       s = state_.channel[ch];
  block_offsets(ch);
  two_d_half(ch, mode);
  auto const left = get(regs_, c.two_d->repeat);
  if(left == 0)
  {
    return false;
  }
  update(regs_, c.two_d->repeat, left - 1);
  update(regs_, c.bytes, s.block_bytes);
  if(mode == 0)
  {
    s.half_reported = false;
    raise_flag(ch, transfer_complete);
  }
  schedule(ch);
  return true;
}

void gpdma::offset(field_ref amount, field_ref decrement, reg_ref target)
{
  auto const value = get(regs_, amount);
  auto const now   = regs_.peek(target.offset);
  regs_.poke(target.offset, get(regs_, decrement) != 0 ? now - value : now + value);
}

void gpdma::block_offsets(std::size_t ch)
{
  auto const& d                        = *ch_[ch].two_d;
  state_.channel[ch].destination_beats = 0;
  offset(d.block_source_offset, d.block_source_decrement, ch_[ch].sar);
  offset(d.block_destination_offset, d.block_destination_decrement, ch_[ch].dar);
}

void gpdma::two_d_half(std::size_t ch, std::uint32_t mode)
{
  auto& s = state_.channel[ch];
  if(mode == 0 or (mode == 3 and regs_.peek(ch_[ch].llr.offset) != 0) or s.half_reported)
  {
    return;
  }
  auto const done = s.repeat - get(regs_, ch_[ch].two_d->repeat) + 1;
  if(done >= (s.repeat + 2u) / 2)
  {
    s.half_reported = true;
    raise_flag(ch, half_transfer);
  }
}

void gpdma::latch(std::size_t ch)
{
  auto const& c = ch_[ch];
  auto&       s = state_.channel[ch];
  s.block_bytes = get(regs_, c.bytes);
  s.repeat      = c.two_d ? static_cast<std::uint16_t>(get(regs_, c.two_d->repeat)) : 0;
}

auto gpdma::link(std::size_t ch) -> bool
{
  auto&      c    = ch_[ch];
  auto&      s    = state_.channel[ch];
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
    return not c.two_d
        or (load(c.two_d->update_tr3, c.two_d->tr3) and load(c.two_d->update_br2, c.two_d->br2));
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
  if(c.update_block.of(llr) == 0)
  {
    update(regs_, c.bytes, s.block_bytes);
    if(c.two_d)
    {
      update(regs_, c.two_d->repeat, s.repeat);
    }
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
