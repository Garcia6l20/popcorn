#include "dma_signal.hpp"
#include "factories.hpp"
#include "model_base.hpp"

#include <popcorn/core/clock.hpp>
#include <popcorn/periph/stm32/audio.hpp>
#include <popcorn/periph/stm32/board.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
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
audio_endpoint::~audio_endpoint()                 = default;
audio_port::~audio_port()                         = default;
audio_capture_endpoint::~audio_capture_endpoint() = default;
audio_source_endpoint::~audio_source_endpoint()   = default;

namespace
{
constexpr std::size_t   blocks        = 2;
constexpr std::size_t   fifo_depth    = 8;
constexpr std::int8_t   no_slot       = -1;
constexpr std::uint64_t ps_per_second = 1'000'000'000'000;

using wide = unsigned __int128;

constexpr std::array<std::uint32_t, 8> data_bits{8, 8, 8, 10, 16, 20, 24, 32};

struct block_state
{
  std::uint32_t              generation{};
  std::vector<std::uint32_t> fifo;
  std::vector<std::uint32_t> frame;
  std::uint64_t              anchor_ps{};
  std::uint64_t              frames{};
  std::uint64_t              next_ps{};
  std::uint64_t              hz{};
  std::uint64_t              cycles{};
  std::uint32_t              bits{};
  std::uint32_t              position{};
  std::int8_t                resume_slot{no_slot};
  std::int8_t                carry_slot{no_slot};
  std::uint32_t              carry_at{};
  std::uint32_t              carry_word{};
  bool                       ticking{};
  bool                       stalled{};
  bool                       active{};
  bool                       stopping{};
  bool                       waiting{};

  friend auto operator==(block_state const&, block_state const&) -> bool = default;
};

struct board_clock
{
  std::uint32_t hz{};
  std::uint64_t anchor_ps{};
  bool          anchored{};

  friend auto operator==(board_clock const&, board_clock const&) -> bool = default;
};

struct sai_state
{
  std::array<block_state, blocks>                block{};
  std::optional<std::array<board_clock, blocks>> clock{};

  friend auto operator==(sai_state const&, sai_state const&) -> bool = default;
};

struct block_fields
{
  field_ref                                    mode;
  field_ref                                    protocol;
  field_ref                                    size;
  field_ref                                    sync;
  field_ref                                    mono;
  field_ref                                    dma;
  field_ref                                    enable;
  field_ref                                    no_divider;
  field_ref                                    divider;
  field_ref                                    oversampling;
  field_ref                                    threshold;
  field_ref                                    flush;
  field_ref                                    mute;
  field_ref                                    companding;
  field_ref                                    frame_length;
  field_ref                                    fs_definition;
  field_ref                                    fs_offset;
  field_ref                                    fs_polarity;
  field_ref                                    fs_active_length;
  field_ref                                    slot_count;
  field_ref                                    slot_size;
  field_ref                                    first_bit_offset;
  field_ref                                    slot_enable;
  field_ref                                    level;
  field_ref                                    request;
  field_ref                                    error;
  field_ref                                    clock_error;
  reg_ref                                      frcr;
  reg_ref                                      data;
  reg_ref                                      clear;
  std::vector<std::pair<field_ref, field_ref>> interrupts;
  std::vector<std::pair<field_ref, field_ref>> clears;
};

struct rate
{
  std::uint64_t hz{};
  std::uint64_t cycles{};
};

struct grid_point
{
  std::uint64_t frames{};
  std::uint64_t at_ps{};
};

constexpr auto index_of(audio_block block) -> std::size_t
{
  return block == audio_block::a ? 0 : 1;
}

auto lower(std::string_view name) -> std::string
{
  std::string out{name};
  std::ranges::transform(
      out, out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

auto requests_of(peer_directory const& peers, std::string_view instance)
    -> std::array<dma_signal, blocks>
{
  auto const prefix = lower(instance);
  return {
      dma_signal{peers, prefix + "_a_dma"},
      dma_signal{peers, prefix + "_b_dma"}
  };
}

class sai;

auto sai_writes() -> std::span<write_entry<sai> const>;

/**
 * @brief SAI1 and SAI2 of the STM32U5, RM0456 section 69 "Serial audio interface (SAI)", audio
 *        blocks A and B in free protocol. A master block's frame lasts MCKDIV x (OSR + 1) x 256
 *        kernel clock cycles with NODIV = 0, MCKDIV x (FRL + 1) with NODIV = 1, MCKDIV 0 counting
 *        as 1, section 69.4.8. A slave block in asynchronous mode runs at the board's external
 *        frame rate, a slave block synchronous with the other block (SYNCEN = 01) frames with it
 *        from the other block's next frame on. The board's decimation stretches every frame. A
 *        frame has FRL + 1 bit times from its frame sync edge. Slots follow each other from bit 0,
 *        or from bit 1 with FSOFF, SLOTSZ bits each, the second half of the slots from the middle
 *        of the frame when FSDEF is set, section 69.4.6. Each slot carries its data from FBOFF bits
 *        into the slot, section 69.4.7, which moves no FIFO access. A frame keeps the timing it
 *        started with across a clock change. A transmitter takes a FIFO word when each active slot
 *        starts, slot 0's word again for slot 1 in mono mode with two slots, and hands the frame to
 *        its endpoint when it ends. A master transmitter starts its first frame when the FIFO first
 *        holds data, section 69.4.3. A receiver takes the frame from its endpoint when its first
 *        active slot ends, and stores each active slot's word when the slot ends, section 69.4.7,
 *        slot 0 only in mono mode. A slot running past the frame end, the last slot of a full frame
 *        with FSOFF, stores its word that many bit times into the next frame, or at the frame end
 *        when no next frame starts, that frame ends first, or a later slot of the same frame also
 *        runs past its end. A block enabled with slots breaking the rules of section 69.4.7, FBOFF
 *        + DS above the slot size, the slots longer than the frame or an odd slot count with FSDEF,
 *        logs it once and runs as configured. A synchronous follower is checked against the frame
 *        of the block it follows, when both run. Endpoints get the frame's start time and its
 *        layout, audio_format. A frame shorter than 8 bits never runs, logged once, section 69.4.6.
 *        Underrun and overrun set OVRUDR and keep the slot position, section 69.4.14: a transmitter
 *        sends 0 and a receiver drops words until the FIFO serves the slot where it happened. FLVL
 *        and FREQ follow the FIFO level against FTH for the block's direction, section 69.4.9, also
 *        while SAIEN is clear. Clearing SAIEN takes effect at the end of the frame in progress,
 *        section 69.4.15. A master with NODIV = 0 and FRL + 1 not a power of 2 from 8 sets WCKCFG
 *        and stays disabled. FRCR.FSDEF is writable, as RM0456 69.6.4 describes it and ST's HAL
 *        writes it, although the register map marks it read-only. The IRQ follows the status flags
 *        of both blocks with their enables. With CR1.DMAEN, the block's DMA request
 *        `<instance>_a_dma` or `<instance>_b_dma` follows FREQ, section 69.4.16, through the GPDMA
 *        peer, also while SAIEN is clear, levels announced again in reset. Not modelled, logged
 *        once: SPDIF, AC'97, PDM, companding, mute, synchronization with another SAI.
 *
 *        The external clock of each block is board state saved in snapshots: a rate and the time of
 *        one of its frame boundaries since power-on. A power-on or brown-out reset powers the codec
 *        down, the clock returns to the board's wired rate with no phase. Other resets keep it
 *        running in phase. A clock started from stopped has a boundary at the start. A rate change
 *        or a stop takes effect at the clock's next frame boundary, whether a block runs on it or
 *        not, so a stop and a start within one frame keep the phase. A slave block enabled while
 *        its clock runs starts at the next boundary, at once on the first enable of a clock with no
 *        phase, which fixes it. The external clock is defined by its frame rate and always matches
 *        the block's frame, so AFSDET and LFSDET never rise, as with a correctly wired codec, and
 *        their interrupt enables log nothing. A state saved before the external clock existed loads
 *        with the clocks of the machine loading it, their phase taken again at the next enable.
 */
class sai final : public dispatch_model<sai, sai_state>,
                  public audio_port,
                  public clock_listener,
                  public dma_feeder,
                  public bus_reset_listener
{
public:
  sai(model_context& ctx, peripheral_desc const& desc, std::array<audio_attachment, blocks> wired);

  void reset(register_file& regs, reset_kind kind) override;
  void on_bus_reset() override;
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
  void               load(std::span<std::byte const> in) override;
  void               attach(audio_block block, audio_attachment attachment) override;
  void               set_external_clock(audio_block which, std::uint32_t frame_hz) override;
  [[nodiscard]] auto external_clock(audio_block which) const -> std::uint32_t override;
  [[nodiscard]] auto quiet_until(quiet_query const& query) const -> std::optional<vtime> override;
  [[nodiscard]] auto
      dma_feed(dma_controller const& by, std::uint32_t request, quiet_query const& query) const
      -> std::optional<feed_info> override;
  [[nodiscard]] auto quiet_data(popcorn::address at, bool write) const -> bool override;

  template <std::size_t B> void control_written(std::uint32_t old, std::uint32_t now);
  template <std::size_t B> void fifo_written(std::uint32_t old, std::uint32_t now);
  template <std::size_t B> void clear_written(std::uint32_t old, std::uint32_t now);
  void                          pdm_written(std::uint32_t old, std::uint32_t now);
  void                          plain_written(std::uint32_t old, std::uint32_t now);

private:
  void               restore_reset_values(std::optional<reset_kind> kind);
  [[nodiscard]] auto fields_of(char prefix) const -> block_fields;
  [[nodiscard]] auto on(field_ref field) const -> bool;
  [[nodiscard]] auto enabled(std::size_t b) const -> bool;
  [[nodiscard]] auto receiver(std::size_t b) const -> bool;
  [[nodiscard]] auto master(std::size_t b) const -> bool;
  [[nodiscard]] auto follows(std::size_t b, std::size_t source) const -> bool;
  [[nodiscard]] auto slots(std::size_t b) const -> std::size_t;
  [[nodiscard]] auto slot_on(std::size_t b, std::size_t slot) const -> bool;
  [[nodiscard]] auto mono(std::size_t b) const -> bool;
  [[nodiscard]] auto mask(std::size_t b) const -> std::uint32_t;
  [[nodiscard]] auto clock_rate(std::size_t b) const -> rate;
  [[nodiscard]] auto frame_rate(std::size_t b) const -> rate;
  [[nodiscard]] auto clocked(std::size_t b) const -> bool;
  [[nodiscard]] auto clock(std::size_t b) -> board_clock&;
  [[nodiscard]] auto board_now() const -> std::uint64_t;
  [[nodiscard]] auto running(std::size_t b) const -> bool;
  [[nodiscard]] auto boundary(std::size_t b, std::uint64_t now) const -> grid_point;
  [[nodiscard]] auto clock_valid(std::size_t b) const -> bool;
  [[nodiscard]] auto format(std::size_t b, std::uint32_t bits) const -> audio_format;
  [[nodiscard]] auto too_short(std::size_t source) -> bool;

  void               enable(std::size_t b);
  void               check_slots(std::size_t b);
  void               disable(std::size_t b);
  [[nodiscard]] auto frame_bits(std::size_t b) const -> std::uint32_t;
  [[nodiscard]] auto slot_width(std::size_t b) const -> std::uint32_t;
  [[nodiscard]] auto slot_start(std::size_t b, std::size_t slot, std::uint32_t bits) const
      -> std::uint32_t;
  [[nodiscard]] auto slot_end(std::size_t b, std::size_t slot, std::uint32_t bits) const
      -> std::uint32_t;
  [[nodiscard]] auto time_at(std::size_t source, std::uint32_t bit) const -> std::uint64_t;

  void               start(std::size_t source, std::uint64_t from_ps);
  void               follow_clock(std::size_t b);
  void               open(std::size_t source);
  void               run(std::size_t source, std::uint32_t position);
  void               begin(std::size_t b);
  void               take(std::size_t b, std::size_t slot);
  [[nodiscard]] auto received(std::size_t b, std::size_t slot, std::size_t source)
      -> std::optional<std::uint32_t>;
  void store(std::size_t b, std::size_t slot, std::size_t source);
  void carry(std::size_t b, std::size_t slot, std::size_t source, std::uint32_t at);
  void flush_carry(std::size_t b);
  void accept(std::size_t b, std::size_t slot, std::uint32_t word);
  void finish(std::size_t b, std::uint32_t bits, std::uint64_t at_ps, std::uint64_t end_ps);
  void halt(std::size_t b);
  void push(std::size_t b, std::uint32_t word);
  void flags();
  void warn_unsupported(std::size_t b);
  [[nodiscard]] auto block_feed(std::size_t b, vtime now) const -> feed_info;

  register_file&                       regs_;
  std::array<audio_attachment, blocks> wired_;
  std::array<block_fields, blocks>     block_;
  field_ref                            pdm_enable_;
  core::clock_domain                   kernel_;
  irq_line&                            line_;
  std::array<dma_signal, blocks>       requests_;
  bool                                 unsupported_logged_{};
  bool                                 no_clock_logged_{};
  bool                                 decimation_logged_{};
  bool                                 short_frame_logged_{};
  bool                                 slot_rules_logged_{};
};

auto sai_writes() -> std::span<write_entry<sai> const>
{
  static constexpr std::array<write_entry<sai>, 19> table{
      {
       {"GCR", &sai::plain_written},    {"ACR1", &sai::control_written<0>},
       {"ACR2", &sai::fifo_written<0>}, {"AFRCR", &sai::plain_written},
       {"ASLOTR", &sai::plain_written}, {"AIM", &sai::plain_written},
       {"ASR", &sai::plain_written},    {"ACLRFR", &sai::clear_written<0>},
       {"ADR", &sai::plain_written},    {"BCR1", &sai::control_written<1>},
       {"BCR2", &sai::fifo_written<1>}, {"BFRCR", &sai::plain_written},
       {"BSLOTR", &sai::plain_written}, {"BIM", &sai::plain_written},
       {"BSR", &sai::plain_written},    {"BCLRFR", &sai::clear_written<1>},
       {"BDR", &sai::plain_written},    {"PDMCR", &sai::pdm_written},
       {"PDMDLY", &sai::plain_written},
       }
  };
  return table;
}

sai::sai(
    model_context&                       ctx,
    peripheral_desc const&               desc,
    std::array<audio_attachment, blocks> wired)
    : dispatch_model{ctx, desc, "SAI", sai_writes()}, regs_{ctx.regs}, wired_{std::move(wired)},
      block_{fields_of('A'), fields_of('B')}, pdm_enable_{field("PDMCR", "PDMEN")},
      kernel_{ctx.clock.domain(lower(desc.name))}, line_{irq(desc.name + "_IRQn")},
      requests_{requests_of(ctx.peers, desc.name)}
{
  auto& clocks = state_.clock.emplace();
  for(std::size_t b = 0; b < blocks; ++b)
  {
    clocks[b].hz = wired_[b].external_frame_hz;
  }
}

auto sai::fields_of(char prefix) const -> block_fields
{
  auto const   name = [prefix](std::string_view reg) { return std::format("{}{}", prefix, reg); };
  auto const   cr1  = name("CR1");
  auto const   cr2  = name("CR2");
  auto const   sr   = name("SR");
  auto const   im   = name("IM");
  auto const   clr  = name("CLRFR");
  block_fields out{
      .mode             = field(cr1, "MODE"),
      .protocol         = field(cr1, "PRTCFG"),
      .size             = field(cr1, "DS"),
      .sync             = field(cr1, "SYNCEN"),
      .mono             = field(cr1, "MONO"),
      .dma              = field(cr1, "DMAEN"),
      .enable           = field(cr1, "SAIAEN"),
      .no_divider       = field(cr1, "NODIV"),
      .divider          = field(cr1, "MCKDIV"),
      .oversampling     = field(cr1, "OSR"),
      .threshold        = field(cr2, "FTH"),
      .flush            = field(cr2, "FFLUSH"),
      .mute             = field(cr2, "MUTE"),
      .companding       = field(cr2, "COMP"),
      .frame_length     = field(name("FRCR"), "FRL"),
      .fs_definition    = field(name("FRCR"), "FSDEF"),
      .fs_offset        = field(name("FRCR"), "FSOFF"),
      .fs_polarity      = field(name("FRCR"), "FSPOL"),
      .fs_active_length = field(name("FRCR"), "FSALL"),
      .slot_count       = field(name("SLOTR"), "NBSLOT"),
      .slot_size        = field(name("SLOTR"), "SLOTSZ"),
      .first_bit_offset = field(name("SLOTR"), "FBOFF"),
      .slot_enable      = field(name("SLOTR"), "SLOTEN"),
      .level            = field(sr, "FLVL"),
      .request          = field(sr, "FREQ"),
      .error            = field(sr, "OVRUDR"),
      .clock_error      = field(sr, "WCKCFG"),
      .frcr             = reg(name("FRCR")),
      .data             = reg(name("DR")),
      .clear            = reg(clr),
      .interrupts       = {},
      .clears           = {}};
  for(auto const& enable : describe(im).fields)
  {
    auto const flag = std::string_view{enable.name}.substr(0, enable.name.size() - 2);
    if(enable.name.ends_with("IE")
       and std::ranges::contains(describe(sr).fields, flag, &field_desc::name))
    {
      out.interrupts.emplace_back(field(sr, flag), field(im, enable.name));
    }
  }
  for(auto const& clear : describe(clr).fields)
  {
    auto const flag = std::string_view{clear.name}.substr(1);
    if(clear.name.starts_with('C')
       and std::ranges::contains(describe(sr).fields, flag, &field_desc::name))
    {
      out.clears.emplace_back(field(clr, clear.name), field(sr, flag));
    }
  }
  return out;
}

auto sai::on(field_ref field) const -> bool
{
  return get(regs_, field) != 0;
}

auto sai::enabled(std::size_t b) const -> bool
{
  return on(block_[b].enable) and not state_.block[b].stopping;
}

auto sai::receiver(std::size_t b) const -> bool
{
  return (get(regs_, block_[b].mode) & 1u) != 0;
}

auto sai::master(std::size_t b) const -> bool
{
  return (get(regs_, block_[b].mode) & 2u) == 0;
}

auto sai::follows(std::size_t b, std::size_t source) const -> bool
{
  return b != source and get(regs_, block_[b].sync) == 1 and get(regs_, block_[source].sync) == 0;
}

auto sai::slots(std::size_t b) const -> std::size_t
{
  return get(regs_, block_[b].slot_count) + 1u;
}

auto sai::slot_on(std::size_t b, std::size_t slot) const -> bool
{
  return ((get(regs_, block_[b].slot_enable) >> slot) & 1u) != 0;
}

auto sai::mono(std::size_t b) const -> bool
{
  return on(block_[b].mono) and slots(b) == 2;
}

auto sai::format(std::size_t b, std::uint32_t bits) const -> audio_format
{
  auto const   used = (1u << slots(b)) - 1u;
  auto const&  f    = block_[b];
  audio_format out{
      .data_bits        = static_cast<std::uint8_t>(data_bits[get(regs_, f.size)]),
      .active_slots     = static_cast<std::uint16_t>(get(regs_, f.slot_enable) & used),
      .mono             = mono(b),
      .frame_bits       = static_cast<std::uint16_t>(bits),
      .slot_bits        = static_cast<std::uint8_t>(slot_width(b)),
      .first_bit_offset = static_cast<std::uint8_t>(get(regs_, f.first_bit_offset)),
      .fs_definition    = on(f.fs_definition),
      .fs_offset        = on(f.fs_offset),
      .fs_active_high   = on(f.fs_polarity),
      .fs_active_bits   = static_cast<std::uint8_t>(get(regs_, f.fs_active_length) + 1u),
      .slot_count       = static_cast<std::uint8_t>(slots(b)),
      .slot_starts      = {}};
  for(std::size_t slot = 0; slot < slots(b); ++slot)
  {
    out.slot_starts[slot] = static_cast<std::uint16_t>(slot_start(b, slot, bits));
  }
  return out;
}

auto sai::too_short(std::size_t source) -> bool
{
  if(frame_bits(source) >= 8)
  {
    return false;
  }
  if(not std::exchange(short_frame_logged_, true))
  {
    warn(
        std::format("{}FRCR", source == 0 ? 'A' : 'B'),
        "frame shorter than 8 bits: the audio block sees no frame");
  }
  return true;
}

auto sai::mask(std::size_t b) const -> std::uint32_t
{
  auto const bits = data_bits[get(regs_, block_[b].size)];
  return bits >= 32 ? ~std::uint32_t{} : (std::uint32_t{1} << bits) - 1u;
}

auto sai::clock_rate(std::size_t b) const -> rate
{
  return {
      .hz     = (*state_.clock)[b].hz,
      .cycles = std::uint64_t{std::max<std::uint32_t>(wired_[b].decimation, 1)}};
}

auto sai::frame_rate(std::size_t b) const -> rate
{
  if(not master(b))
  {
    return clock_rate(b);
  }
  auto const decimation = std::uint64_t{std::max<std::uint32_t>(wired_[b].decimation, 1)};
  auto const divider    = std::uint64_t{std::max<std::uint32_t>(get(regs_, block_[b].divider), 1)};
  auto const bits       = std::uint64_t{get(regs_, block_[b].frame_length)} + 1u;
  auto const cycles = on(block_[b].no_divider)
                        ? divider * bits
                        : divider * (std::uint64_t{get(regs_, block_[b].oversampling)} + 1u) * 256u;
  return {.hz = ctx_.clock.hz(kernel_), .cycles = cycles * decimation};
}

auto sai::clocked(std::size_t b) const -> bool
{
  return not master(b) and get(regs_, block_[b].sync) == 0;
}

auto sai::clock(std::size_t b) -> board_clock&
{
  return (*state_.clock)[b];
}

auto sai::board_now() const -> std::uint64_t
{
  return ctx_.clock.since_power_on().ps;
}

auto sai::running(std::size_t b) const -> bool
{
  auto const& c = (*state_.clock)[b];
  return c.hz != 0
      or (c.anchored and board_now() < c.anchor_ps)
      or (state_.block[b].ticking and clocked(b));
}

auto sai::boundary(std::size_t b, std::uint64_t now) const -> grid_point
{
  auto const& c = (*state_.clock)[b];
  auto const  r = clock_rate(b);
  if(now <= c.anchor_ps or r.hz == 0)
  {
    return {.frames = 0, .at_ps = c.anchor_ps};
  }
  auto const period = static_cast<wide>(r.cycles) * ps_per_second;
  auto const frames = (static_cast<wide>(now - c.anchor_ps) * r.hz + period - 1) / period;
  return {
      .frames = static_cast<std::uint64_t>(frames),
      .at_ps  = c.anchor_ps + static_cast<std::uint64_t>(frames * period / r.hz)};
}

auto sai::clock_valid(std::size_t b) const -> bool
{
  auto const bits         = get(regs_, block_[b].frame_length) + 1u;
  auto const power_of_two = (bits & (bits - 1u)) == 0;
  auto const checked      = master(b) and not on(block_[b].no_divider);
  return not checked or (power_of_two and bits >= 8);
}

void sai::reset(register_file&, reset_kind kind)
{
  restore_reset_values(kind);
}

void sai::on_bus_reset()
{
  restore_reset_values(std::nullopt);
}

void sai::restore_reset_values(std::optional<reset_kind> kind)
{
  for(auto const& name : claims())
  {
    regs_.poke(reg(name).offset, describe(name).reset);
  }
  for(auto& block : state_.block)
  {
    auto const generation = block.generation + 1;
    block                 = {};
    block.generation      = generation;
  }
  if(kind == reset_kind::power_on or kind == reset_kind::brown_out)
  {
    for(std::size_t b = 0; b < blocks; ++b)
    {
      clock(b) = {.hz = wired_[b].external_frame_hz};
    }
  }
  for(auto& request : requests_)
  {
    request.resolve();
  }
  flags();
}

void sai::warn_unsupported(std::size_t b)
{
  auto const& f          = block_[b];
  auto const  protocol   = get(regs_, f.protocol) != 0;
  auto const  companding = get(regs_, f.companding) != 0;
  auto const  external   = get(regs_, f.sync) >= 2;
  auto const  pdm        = get(regs_, pdm_enable_) != 0;
  auto const  used       = protocol or companding or on(f.mute) or external or pdm;
  if(used and not std::exchange(unsupported_logged_, true))
  {
    warn(
        std::format("{}CR1", b == 0 ? 'A' : 'B'),
        "SPDIF, AC'97, PDM, companding, mute or synchronization with another SAI is configured, "
        "not modelled");
  }
}

template <std::size_t B> void sai::control_written(std::uint32_t old, std::uint32_t now)
{
  auto const& enable_bit = block_[B].enable;
  auto const  was        = enable_bit.of(old) != 0;
  auto const  is         = enable_bit.of(now) != 0;
  warn_unsupported(B);
  if(is and not was)
  {
    enable(B);
  }
  else if(was and not is)
  {
    disable(B);
  }
  flags();
}

template <std::size_t B> void sai::fifo_written(std::uint32_t, std::uint32_t now)
{
  if(block_[B].flush.of(now) != 0)
  {
    state_.block[B].fifo.clear();
    update(regs_, block_[B].flush, 0);
  }
  warn_unsupported(B);
  flags();
}

template <std::size_t B> void sai::clear_written(std::uint32_t, std::uint32_t now)
{
  for(auto const& [clear, flag] : block_[B].clears)
  {
    if(clear.of(now) != 0)
    {
      update(regs_, flag, 0);
    }
  }
  regs_.poke(block_[B].clear.offset, 0);
  flags();
}

void sai::pdm_written(std::uint32_t, std::uint32_t)
{
  warn_unsupported(0);
}

void sai::plain_written(std::uint32_t, std::uint32_t)
{
  flags();
}

void sai::enable(std::size_t b)
{
  if(not clock_valid(b))
  {
    update(regs_, block_[b].clock_error, 1);
    update(regs_, block_[b].enable, 0);
    return;
  }
  if(not follows(b, 1 - b) or state_.block[1 - b].ticking)
  {
    check_slots(b);
  }
  if(get(regs_, block_[b].sync) != 0)
  {
    return;
  }
  auto& block = state_.block[b];
  if(master(b) and not receiver(b) and block.fifo.empty())
  {
    block.waiting = true;
    return;
  }
  if(master(b))
  {
    start(b, ctx_.clock.time().ps);
    return;
  }
  follow_clock(b);
}

void sai::follow_clock(std::size_t b)
{
  auto& c = clock(b);
  if(c.hz == 0)
  {
    start(b, ctx_.clock.time().ps);
    return;
  }
  auto const now = board_now();
  if(not c.anchored)
  {
    c.anchor_ps = now;
    c.anchored  = true;
  }
  c.anchor_ps = boundary(b, now).at_ps;
  start(b, c.anchor_ps - (now - ctx_.clock.time().ps));
}

void sai::disable(std::size_t b)
{
  auto& block = state_.block[b];
  if(block.active)
  {
    block.stopping = true;
    update(regs_, block_[b].enable, 1);
    return;
  }
  halt(b);
}

void sai::start(std::size_t source, std::uint64_t from_ps)
{
  auto& block   = state_.block[source];
  block.waiting = false;
  block.stalled = false;
  auto const r  = frame_rate(source);
  if(r.hz == 0)
  {
    block.stalled    = true;
    auto const wired = not master(source) and wired_[source].clock_wired;
    if(not wired and not std::exchange(no_clock_logged_, true))
    {
      warn(
          std::format("{}CR1", source == 0 ? 'A' : 'B'),
          master(source) ? "no kernel clock: the audio block sees no frame"
                         : "slave block without an external frame rate: it sees no frame");
    }
    return;
  }
  if(too_short(source))
  {
    return;
  }
  if(wired_[source].decimation > 1 and not std::exchange(decimation_logged_, true))
  {
    warn(
        std::format("{}CR1", source == 0 ? 'A' : 'B'),
        std::format("frames decimated by {}: frame time stretched", wired_[source].decimation));
  }
  block.anchor_ps = from_ps;
  block.frames    = 0;
  block.hz        = r.hz;
  block.cycles    = r.cycles;
  block.bits      = frame_bits(source);
  block.ticking   = true;
  for(std::size_t b = 0; b < blocks; ++b)
  {
    if(follows(b, source) and enabled(b))
    {
      check_slots(b);
    }
  }
  if(auto const first = time_at(source, 0); first > ctx_.clock.time().ps)
  {
    block.position = 0;
    block.next_ps  = first;
    ctx_.events.schedule_at(
        vtime{first}, static_cast<std::uint32_t>(source), block.generation, ctx_.clock);
    return;
  }
  for(std::size_t b = 0; b < blocks; ++b)
  {
    if((b == source or follows(b, source)) and enabled(b))
    {
      begin(b);
    }
  }
  run(source, 0);
}

void sai::open(std::size_t source)
{
  auto&      block = state_.block[source];
  auto const r     = frame_rate(source);
  if(r.hz == block.hz and r.cycles == block.cycles and frame_bits(source) == block.bits)
  {
    for(std::size_t b = 0; b < blocks; ++b)
    {
      if((b == source or follows(b, source)) and enabled(b))
      {
        begin(b);
      }
    }
    run(source, 0);
    return;
  }
  auto const at = block.next_ps;
  block.ticking = false;
  if(clocked(source) and r.hz != 0)
  {
    clock(source).anchor_ps = at + board_now() - ctx_.clock.time().ps;
    clock(source).anchored  = true;
  }
  start(source, at);
}

auto sai::frame_bits(std::size_t b) const -> std::uint32_t
{
  return get(regs_, block_[b].frame_length) + 1u;
}

auto sai::slot_width(std::size_t b) const -> std::uint32_t
{
  auto const code = get(regs_, block_[b].slot_size);
  return code == 1 ? 16u : (code == 2 ? 32u : data_bits[get(regs_, block_[b].size)]);
}

auto sai::slot_start(std::size_t b, std::size_t slot, std::uint32_t bits) const -> std::uint32_t
{
  auto const width  = slot_width(b);
  auto const count  = slots(b);
  auto const index  = static_cast<std::uint32_t>(slot);
  auto const offset = on(block_[b].fs_offset) ? 1u : 0u;
  if(on(block_[b].fs_definition) and count >= 2)
  {
    auto const half = static_cast<std::uint32_t>(count / 2);
    return offset + (index < half ? index * width : bits / 2 + (index - half) * width);
  }
  return offset + index * width;
}

auto sai::slot_end(std::size_t b, std::size_t slot, std::uint32_t bits) const -> std::uint32_t
{
  return slot_start(b, slot, bits) + slot_width(b);
}

void sai::check_slots(std::size_t b)
{
  auto const width = slot_width(b);
  auto const data  = data_bits[get(regs_, block_[b].size)];
  auto const first = get(regs_, block_[b].first_bit_offset);
  auto const bits  = get(regs_, block_[b].sync) == 1 ? frame_bits(1 - b) : frame_bits(b);
  auto const fits  = data <= width and first <= width - data;
  auto const room  = slots(b) * width <= bits;
  auto const even  = not on(block_[b].fs_definition) or slots(b) % 2 == 0;
  if((fits and room and even) or std::exchange(slot_rules_logged_, true))
  {
    return;
  }
  warn(
      std::format("{}SLOTR", b == 0 ? 'A' : 'B'),
      std::format(
          "slots break RM0456 69.4.7: FBOFF {} with {} data bits in {}-bit slots, {} slots in a "
          "{}-bit frame, FSDEF {}",
          first, data, width, slots(b), bits, on(block_[b].fs_definition) ? 1 : 0));
}

auto sai::time_at(std::size_t source, std::uint32_t bit) const -> std::uint64_t
{
  auto const& block = state_.block[source];
  auto const  count = static_cast<wide>(block.frames) * block.bits + bit;
  auto const  span =
      count * block.cycles * ps_per_second / (static_cast<wide>(block.hz) * block.bits);
  return block.anchor_ps + static_cast<std::uint64_t>(span);
}

void sai::run(std::size_t source, std::uint32_t position)
{
  auto&                    block = state_.block[source];
  std::array<bool, blocks> members{};
  for(;;)
  {
    auto const bits = block.bits;
    for(std::size_t b = 0; b < blocks; ++b)
    {
      members[b] = b == source or follows(b, source);
      if(not members[b] or not state_.block[b].active)
      {
        continue;
      }
      auto const& member = state_.block[b];
      if(member.carry_slot != no_slot and (member.carry_at == position or position == bits))
      {
        flush_carry(b);
      }
      for(std::size_t slot = 0; slot < slots(b); ++slot)
      {
        auto const fits = slot_start(b, slot, bits) < bits;
        if(not slot_on(b, slot) or not fits)
        {
          continue;
        }
        if(receiver(b))
        {
          auto const end = slot_end(b, slot, bits);
          if(end > bits and position == bits)
          {
            carry(b, slot, source, end - bits);
          }
          else if(end == position)
          {
            store(b, slot, source);
          }
        }
        else if(not receiver(b) and slot_start(b, slot, bits) == position)
        {
          take(b, slot);
        }
      }
    }
    if(position < bits)
    {
      break;
    }
    auto const start_ps = time_at(source, 0);
    auto const end_ps   = time_at(source, bits);
    for(std::size_t b = 0; b < blocks; ++b)
    {
      if(members[b] and state_.block[b].active)
      {
        finish(b, bits, start_ps, end_ps);
      }
    }
    for(std::size_t b = 0; b < blocks; ++b)
    {
      if(members[b] and state_.block[b].stopping)
      {
        halt(b);
      }
    }
    auto const r = frame_rate(source);
    if(clocked(source) and r.hz != 0 and (r.hz != block.hz or r.cycles != block.cycles))
    {
      clock(source).anchor_ps = end_ps + board_now() - ctx_.clock.time().ps;
      clock(source).anchored  = true;
    }
    if(not enabled(source) or r.hz == 0 or too_short(source))
    {
      for(std::size_t b = 0; b < blocks; ++b)
      {
        if(members[b])
        {
          flush_carry(b);
        }
      }
      block.ticking = false;
      block.stalled = enabled(source) and r.hz == 0;
      return;
    }
    if(r.hz != block.hz or r.cycles != block.cycles or frame_bits(source) != block.bits)
    {
      block.anchor_ps = end_ps;
      block.frames    = 0;
      block.hz        = r.hz;
      block.cycles    = r.cycles;
      block.bits      = frame_bits(source);
    }
    else
    {
      ++block.frames;
    }
    for(std::size_t b = 0; b < blocks; ++b)
    {
      if(members[b] and enabled(b))
      {
        begin(b);
      }
      else if(members[b])
      {
        flush_carry(b);
      }
    }
    position = 0;
  }
  auto next = block.bits;
  for(std::size_t b = 0; b < blocks; ++b)
  {
    if(not members[b] or not state_.block[b].active)
    {
      continue;
    }
    auto const& member = state_.block[b];
    if(member.carry_slot != no_slot and member.carry_at > position and member.carry_at < next)
    {
      next = member.carry_at;
    }
    for(std::size_t slot = 0; slot < slots(b); ++slot)
    {
      if(not slot_on(b, slot))
      {
        continue;
      }
      auto const point = receiver(b) ? std::min(slot_end(b, slot, block.bits), block.bits)
                                     : slot_start(b, slot, block.bits);
      if(point > position and point < next)
      {
        next = point;
      }
    }
  }
  block.position = next;
  block.next_ps  = time_at(source, next);
  ctx_.events.schedule_at(
      vtime{block.next_ps}, static_cast<std::uint32_t>(source), block.generation, ctx_.clock);
}

void sai::begin(std::size_t b)
{
  auto& block  = state_.block[b];
  block.active = true;
  block.frame.assign(receiver(b) ? 0 : slots(b), 0);
}

void sai::take(std::size_t b, std::size_t slot)
{
  auto& block = state_.block[b];
  if(slot >= block.frame.size())
  {
    return;
  }
  if(slot == 1 and mono(b))
  {
    block.frame[1] = block.frame[0];
    return;
  }
  auto const here = static_cast<std::int8_t>(slot);
  if(block.resume_slot != no_slot and block.resume_slot != here)
  {
    return;
  }
  if(block.fifo.empty())
  {
    update(regs_, block_[b].error, 1);
    block.resume_slot = here;
    return;
  }
  block.resume_slot = no_slot;
  block.frame[slot] = block.fifo.front() & mask(b);
  block.fifo.erase(block.fifo.begin());
}

auto sai::received(std::size_t b, std::size_t slot, std::size_t source)
    -> std::optional<std::uint32_t>
{
  auto& block = state_.block[b];
  if(block.frame.empty())
  {
    block.frame.assign(slots(b), 0);
    if(auto const far_end = wired_[b].endpoint)
    {
      far_end->receive(
          block.frame, format(b, state_.block[source].bits), vtime{time_at(source, 0)});
    }
  }
  if(slot >= block.frame.size() or (slot == 1 and mono(b)))
  {
    return std::nullopt;
  }
  return block.frame[slot];
}

void sai::store(std::size_t b, std::size_t slot, std::size_t source)
{
  if(auto const word = received(b, slot, source))
  {
    accept(b, slot, *word);
  }
}

void sai::carry(std::size_t b, std::size_t slot, std::size_t source, std::uint32_t at)
{
  flush_carry(b);
  if(auto const word = received(b, slot, source))
  {
    auto& block      = state_.block[b];
    block.carry_slot = static_cast<std::int8_t>(slot);
    block.carry_at   = at;
    block.carry_word = *word;
  }
}

void sai::flush_carry(std::size_t b)
{
  auto& block = state_.block[b];
  if(block.carry_slot == no_slot)
  {
    return;
  }
  auto const slot = static_cast<std::size_t>(std::exchange(block.carry_slot, no_slot));
  auto const word = std::exchange(block.carry_word, 0);
  block.carry_at  = 0;
  accept(b, slot, word);
}

void sai::accept(std::size_t b, std::size_t slot, std::uint32_t word)
{
  auto&      block = state_.block[b];
  auto const here  = static_cast<std::int8_t>(slot);
  if(block.resume_slot != no_slot and block.resume_slot != here)
  {
    return;
  }
  if(block.fifo.size() >= fifo_depth)
  {
    update(regs_, block_[b].error, 1);
    block.resume_slot = here;
    return;
  }
  block.resume_slot = no_slot;
  block.fifo.push_back(word & mask(b));
}

void sai::finish(std::size_t b, std::uint32_t bits, std::uint64_t at_ps, std::uint64_t end_ps)
{
  auto& block        = state_.block[b];
  block.active       = false;
  auto const frame   = std::exchange(block.frame, {});
  auto const far_end = wired_[b].endpoint;
  if(not receiver(b) and far_end)
  {
    far_end->transmitted(frame, format(b, bits), vtime{at_ps}, vtime{end_ps});
  }
}

void sai::halt(std::size_t b)
{
  auto& block = state_.block[b];
  ++block.generation;
  block.active   = false;
  block.stopping = false;
  block.waiting  = false;
  block.ticking  = false;
  block.stalled  = false;
  block.frame.clear();
  update(regs_, block_[b].enable, 0);
}

void sai::on_event(std::uint32_t kind, std::uint64_t payload)
{
  if(kind >= blocks or payload != state_.block[kind].generation or not state_.block[kind].ticking)
  {
    return;
  }
  if(state_.block[kind].position == 0)
  {
    open(kind);
  }
  else
  {
    run(kind, state_.block[kind].position);
  }
  flags();
}

void sai::on_clock_change()
{
  for(std::size_t b = 0; b < blocks; ++b)
  {
    if(state_.block[b].stalled and enabled(b) and master(b) and frame_rate(b).hz != 0)
    {
      start(b, ctx_.clock.time().ps);
    }
  }
  flags();
}

void sai::push(std::size_t b, std::uint32_t word)
{
  auto& block = state_.block[b];
  if(receiver(b) or block.fifo.size() >= fifo_depth)
  {
    return;
  }
  block.fifo.push_back(word);
  if(block.waiting)
  {
    start(b, ctx_.clock.time().ps);
  }
}

auto sai::on_read(register_file& regs, std::uint32_t offset, std::uint8_t size, access_ctx ctx)
    -> std::expected<std::uint32_t, bus_error>
{
  auto const word = offset & ~3u;
  for(std::size_t b = 0; b < blocks; ++b)
  {
    if(word != block_[b].data.offset)
    {
      continue;
    }
    auto& fifo = state_.block[b].fifo;
    if(not receiver(b) or fifo.empty())
    {
      return 0u;
    }
    auto const value = fifo.front();
    if(ctx.by != initiator::debugger)
    {
      fifo.erase(fifo.begin());
      flags();
    }
    return read_lanes(value, offset, size);
  }
  return dispatch_model::on_read(regs, offset, size, ctx);
}

auto sai::on_write(
    register_file& regs,
    std::uint32_t  offset,
    std::uint8_t   size,
    std::uint32_t  old,
    std::uint32_t  value,
    access_ctx     ctx) -> std::expected<void, bus_error>
{
  auto const word = offset & ~3u;
  for(std::size_t b = 0; b < blocks; ++b)
  {
    if(word == block_[b].data.offset)
    {
      push(b, merge_write(0, offset, size, value));
      flags();
      return {};
    }
    if(word == block_[b].frcr.offset)
    {
      auto const before  = regs.peek(word);
      auto const written = merge_write(before, offset, size, value);
      auto const result  = dispatch_model::on_write(regs, offset, size, old, value, ctx);
      update(regs, block_[b].fs_definition, block_[b].fs_definition.of(written));
      return result;
    }
  }
  return dispatch_model::on_write(regs, offset, size, old, value, ctx);
}

void sai::attach(audio_block block, audio_attachment attachment)
{
  auto const b  = index_of(block);
  wired_[b]     = std::move(attachment);
  auto const hz = wired_[b].external_frame_hz;
  if(hz == 0 or running(b))
  {
    set_external_clock(block, hz);
    return;
  }
  clock(b) = {.hz = hz};
  if(state_.block[b].stalled and enabled(b) and clocked(b))
  {
    follow_clock(b);
    flags();
  }
}

void sai::set_external_clock(audio_block which, std::uint32_t frame_hz)
{
  auto const b   = index_of(which);
  auto&      c   = clock(b);
  auto const now = board_now();
  if(c.hz == frame_hz)
  {
    return;
  }
  if(not running(b))
  {
    c = {.hz = frame_hz, .anchor_ps = now, .anchored = true};
  }
  else
  {
    if(c.anchored)
    {
      c.anchor_ps = boundary(b, now).at_ps;
    }
    c.hz = frame_hz;
  }
  if(frame_hz != 0 and state_.block[b].stalled and enabled(b) and clocked(b))
  {
    follow_clock(b);
    flags();
  }
}

auto sai::external_clock(audio_block which) const -> std::uint32_t
{
  return (*state_.clock)[index_of(which)].hz;
}

void sai::load(std::span<std::byte const> in)
{
  auto kept = state_.clock;
  dispatch_model::load(in);
  if(not state_.clock)
  {
    for(auto& c : *kept)
    {
      c.anchored = false;
    }
    state_.clock = kept;
  }
}

auto sai::block_feed(std::size_t b, vtime now) const -> feed_info
{
  auto const  source = follows(b, 1 - b) ? 1 - b : b;
  auto const  count  = state_.block[b].fifo.size();
  auto const& src    = state_.block[source];
  feed_info   info{.room = static_cast<std::uint32_t>(receiver(b) ? count : fifo_depth - count)};
  if(src.waiting)
  {
    info.next = now;
    return info;
  }
  if(not src.ticking)
  {
    return info;
  }
  info.next = vtime{src.next_ps};
  std::vector<std::uint32_t> points;
  if(state_.block[b].carry_slot != no_slot)
  {
    points.push_back(state_.block[b].carry_at);
  }
  for(std::size_t slot = 0; slot < slots(b); ++slot)
  {
    if(slot_on(b, slot))
    {
      points.push_back(
          std::min(
              receiver(b) ? slot_end(b, slot, src.bits) : slot_start(b, slot, src.bits), src.bits));
    }
  }
  std::ranges::sort(points);
  auto          gap_bits = src.bits;
  std::uint32_t same     = 1;
  for(std::size_t i = 1; i < points.size(); ++i)
  {
    if(points[i] > points[i - 1])
    {
      gap_bits = std::min(gap_bits, points[i] - points[i - 1]);
      same     = 1;
    }
    else
    {
      info.per_event = std::max(info.per_event, ++same);
    }
  }
  if(not points.empty())
  {
    gap_bits =
        std::min(gap_bits, std::max<std::uint32_t>(1, src.bits - points.back() + points.front()));
  }
  auto const span = time_at(source, gap_bits) - time_at(source, 0);
  info.gap_ps     = span > 1 ? span - 1 : 1;
  return info;
}

auto sai::dma_feed(dma_controller const& by, std::uint32_t request, quiet_query const& query) const
    -> std::optional<feed_info>
{
  for(std::size_t b = 0; b < blocks; ++b)
  {
    if(requests_[b].carries(by, request))
    {
      return block_feed(b, query.now());
    }
  }
  return std::nullopt;
}

auto sai::quiet_data(popcorn::address at, bool) const -> bool
{
  auto const offset = window_offset(ctx_.part, desc_, at);
  return offset and std::ranges::any_of(block_, [&](block_fields const& f) {
           return (*offset & ~3u) == f.data.offset;
         });
}

auto sai::quiet_until(quiet_query const& query) const -> std::optional<vtime>
{
  if(line_.level() or not query.wakes(line_))
  {
    return std::nullopt;
  }
  auto const same = [](field_ref const& a, field_ref const& b) {
    return a.offset == b.offset and a.mask() == b.mask();
  };
  std::optional<vtime> out;
  auto const           lower = [&](std::uint64_t ps) {
    if(not out or ps < out->ps)
    {
      out = vtime{ps};
    }
  };
  for(std::size_t b = 0; b < blocks; ++b)
  {
    auto const& f      = block_[b];
    auto        error  = false;
    auto        status = false;
    for(auto const& [flag, enable] : f.interrupts)
    {
      if(get(regs_, enable) != 0)
      {
        error  = error or same(flag, f.error);
        status = status or same(flag, f.request);
      }
    }
    if(not error and not status)
    {
      continue;
    }
    auto const feed = block_feed(b, query.now());
    if(not feed.next)
    {
      continue;
    }
    auto const next = feed.next->ps;
    if(status)
    {
      lower(next);
    }
    if(not error)
    {
      continue;
    }
    auto const    count = state_.block[b].fifo.size();
    std::uint64_t bound = receiver(b) ? fifo_depth - count : count;
    if(get(regs_, f.dma) != 0)
    {
      auto const supply = requests_[b].supply();
      if(not supply.delay_ps or *supply.delay_ps >= feed.gap_ps)
      {
        lower(next);
        continue;
      }
      if(not supply.words)
      {
        continue;
      }
      bound += *supply.words;
    }
    auto const events = bound / feed.per_event + 1;
    lower(next + (events - 1) * feed.gap_ps);
  }
  return out;
}

void sai::flags()
{
  auto level = false;
  for(std::size_t b = 0; b < blocks; ++b)
  {
    auto const&   f         = block_[b];
    auto const    count     = state_.block[b].fifo.size();
    auto const    threshold = std::min<std::uint32_t>(get(regs_, f.threshold), 4);
    auto const    rx        = receiver(b);
    std::uint32_t fill      = 0;
    if(count >= fifo_depth)
    {
      fill = 5;
    }
    else if(count != 0)
    {
      fill = rx ? static_cast<std::uint32_t>(count / 2 + 1)
                : static_cast<std::uint32_t>((count + 1) / 2);
    }
    update(regs_, f.level, fill);
    update(regs_, f.request, (rx ? fill > threshold : fill <= threshold) ? 1 : 0);
    for(auto const& [flag, enable] : f.interrupts)
    {
      level = level or (get(regs_, flag) != 0 and get(regs_, enable) != 0);
    }
  }
  if(level)
  {
    line_.raise();
  }
  else
  {
    line_.lower();
  }
  for(std::size_t b = 0; b < blocks; ++b)
  {
    requests_[b].set(on(block_[b].dma) and on(block_[b].request));
  }
}
}

auto sai_factory(board const& b) -> model_factory
{
  return
      [audio = b.audio](model_context& ctx, peripheral_desc const& desc) -> std::shared_ptr<model> {
        std::array<audio_attachment, 2> wired{};
        for(std::size_t index = 0; index < wired.size(); ++index)
        {
          auto const name = std::format("{}_{}", desc.name, index == 0 ? 'A' : 'B');
          if(auto const found = audio.find(name); found != audio.end())
          {
            wired[index] = found->second;
          }
        }
        return std::make_shared<sai>(ctx, desc, std::move(wired));
      };
}
}
