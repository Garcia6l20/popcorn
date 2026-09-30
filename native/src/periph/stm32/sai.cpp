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
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace popcorn::periph::stm32
{
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
  bool                       ticking{};
  bool                       stalled{};
  bool                       active{};
  bool                       stopping{};
  bool                       waiting{};

  friend auto operator==(block_state const&, block_state const&) -> bool = default;
};

struct sai_state
{
  std::array<block_state, blocks> block{};

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
  field_ref                                    slot_count;
  field_ref                                    slot_size;
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
 *        blocks A and B in free protocol. A master block's frame lasts
 *        MCKDIV x (OSR + 1) x 256 kernel clock cycles with NODIV = 0, MCKDIV x (FRL + 1) with
 *        NODIV = 1, MCKDIV 0 counting as 1, section 69.4.8. A slave block in asynchronous mode
 *        runs at the board's external frame rate, a slave block synchronous with the other block
 *        (SYNCEN = 01) frames with it from the other block's next frame on. The board's
 *        decimation stretches every frame. A frame has FRL + 1 bit times. Slots follow each other
 *        from bit 0, SLOTSZ bits each, the second half of the slots from the middle of the frame
 *        when FSDEF is set. A frame keeps the timing it started with across a clock change. A
 *        transmitter takes a FIFO word when each active slot starts, slot 0's word again for slot
 *        1 in mono mode with two slots, and hands the frame to its endpoint when it ends. A master
 *        transmitter starts its first frame when the FIFO first holds data, section 69.4.3. A
 *        receiver takes the frame from its endpoint when its first active slot ends, and stores
 *        each active slot's word when the slot ends, slot 0 only in mono mode. Endpoints get the
 *        frame's start time. A frame shorter than 8 bits never runs, logged once, section 69.4.6.
 *        Underrun and overrun set OVRUDR and keep the slot position, section 69.4.14: a
 *        transmitter sends 0 and a receiver drops words until the FIFO serves the slot where it
 *        happened. FLVL and FREQ follow the FIFO level against FTH for the block's direction,
 *        section 69.4.9, also while SAIEN is clear. Clearing SAIEN takes effect at the end of the
 *        frame in progress, section 69.4.15. A master with NODIV = 0 and FRL + 1 not a power of 2
 *        from 8 sets WCKCFG and stays disabled. FRCR.FSDEF is writable, as RM0456 69.6.4
 *        describes it and ST's HAL writes it, although the register map marks it read-only. The
 *        IRQ follows the status flags of both blocks with their enables. With CR1.DMAEN, the
 *        block's DMA request `<instance>_a_dma` or `<instance>_b_dma` follows FREQ, section
 *        69.4.16, through the GPDMA peer, also while SAIEN is clear, levels announced again in
 *        reset. Not modelled, logged once: SPDIF, AC'97, PDM, companding, mute, synchronization
 *        with another SAI. The frame and slot timing ignore FSOFF and FBOFF.
 */
class sai final : public dispatch_model<sai, sai_state>, public audio_port, public clock_listener
{
public:
  sai(model_context& ctx, peripheral_desc const& desc, std::array<audio_attachment, blocks> wired);

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
  void attach(audio_block block, audio_attachment attachment) override;

  template <std::size_t B> void control_written(std::uint32_t old, std::uint32_t now);
  template <std::size_t B> void fifo_written(std::uint32_t old, std::uint32_t now);
  template <std::size_t B> void clear_written(std::uint32_t old, std::uint32_t now);
  void                          pdm_written(std::uint32_t old, std::uint32_t now);
  void                          plain_written(std::uint32_t old, std::uint32_t now);

private:
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
  [[nodiscard]] auto frame_rate(std::size_t b) const -> rate;
  [[nodiscard]] auto clock_valid(std::size_t b) const -> bool;
  [[nodiscard]] auto format(std::size_t b) const -> audio_format;
  [[nodiscard]] auto too_short(std::size_t source) -> bool;

  void               enable(std::size_t b);
  void               disable(std::size_t b);
  [[nodiscard]] auto frame_bits(std::size_t b) const -> std::uint32_t;
  [[nodiscard]] auto slot_start(std::size_t b, std::size_t slot, std::uint32_t bits) const
      -> std::uint32_t;
  [[nodiscard]] auto slot_end(std::size_t b, std::size_t slot, std::uint32_t bits) const
      -> std::uint32_t;
  [[nodiscard]] auto time_at(std::size_t source, std::uint32_t bit) const -> std::uint64_t;

  void start(std::size_t source, std::uint64_t from_ps);
  void run(std::size_t source, std::uint32_t position);
  void begin(std::size_t b);
  void take(std::size_t b, std::size_t slot);
  void store(std::size_t b, std::size_t slot, std::size_t source);
  void finish(std::size_t b, std::uint64_t at_ps);
  void halt(std::size_t b);
  void push(std::size_t b, std::uint32_t word);
  void flags();
  void warn_unsupported(std::size_t b);

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
{}

auto sai::fields_of(char prefix) const -> block_fields
{
  auto const   name = [prefix](std::string_view reg) { return std::format("{}{}", prefix, reg); };
  auto const   cr1  = name("CR1");
  auto const   cr2  = name("CR2");
  auto const   sr   = name("SR");
  auto const   im   = name("IM");
  auto const   clr  = name("CLRFR");
  block_fields out{
      .mode          = field(cr1, "MODE"),
      .protocol      = field(cr1, "PRTCFG"),
      .size          = field(cr1, "DS"),
      .sync          = field(cr1, "SYNCEN"),
      .mono          = field(cr1, "MONO"),
      .dma           = field(cr1, "DMAEN"),
      .enable        = field(cr1, "SAIAEN"),
      .no_divider    = field(cr1, "NODIV"),
      .divider       = field(cr1, "MCKDIV"),
      .oversampling  = field(cr1, "OSR"),
      .threshold     = field(cr2, "FTH"),
      .flush         = field(cr2, "FFLUSH"),
      .mute          = field(cr2, "MUTE"),
      .companding    = field(cr2, "COMP"),
      .frame_length  = field(name("FRCR"), "FRL"),
      .fs_definition = field(name("FRCR"), "FSDEF"),
      .slot_count    = field(name("SLOTR"), "NBSLOT"),
      .slot_size     = field(name("SLOTR"), "SLOTSZ"),
      .slot_enable   = field(name("SLOTR"), "SLOTEN"),
      .level         = field(sr, "FLVL"),
      .request       = field(sr, "FREQ"),
      .error         = field(sr, "OVRUDR"),
      .clock_error   = field(sr, "WCKCFG"),
      .frcr          = reg(name("FRCR")),
      .data          = reg(name("DR")),
      .clear         = reg(clr),
      .interrupts    = {},
      .clears        = {}};
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

auto sai::format(std::size_t b) const -> audio_format
{
  auto const used = (1u << slots(b)) - 1u;
  return {
      .data_bits    = static_cast<std::uint8_t>(data_bits[get(regs_, block_[b].size)]),
      .active_slots = static_cast<std::uint16_t>(get(regs_, block_[b].slot_enable) & used),
      .mono         = mono(b)};
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

auto sai::frame_rate(std::size_t b) const -> rate
{
  auto const& wiring     = wired_[b];
  auto const  decimation = std::uint64_t{std::max<std::uint32_t>(wiring.decimation, 1)};
  if(not master(b))
  {
    return {.hz = wiring.external_frame_hz, .cycles = decimation};
  }
  auto const divider = std::uint64_t{std::max<std::uint32_t>(get(regs_, block_[b].divider), 1)};
  auto const bits    = std::uint64_t{get(regs_, block_[b].frame_length)} + 1u;
  auto const cycles = on(block_[b].no_divider)
                        ? divider * bits
                        : divider * (std::uint64_t{get(regs_, block_[b].oversampling)} + 1u) * 256u;
  return {.hz = ctx_.clock.hz(kernel_), .cycles = cycles * decimation};
}

auto sai::clock_valid(std::size_t b) const -> bool
{
  auto const bits         = get(regs_, block_[b].frame_length) + 1u;
  auto const power_of_two = (bits & (bits - 1u)) == 0;
  auto const checked      = master(b) and not on(block_[b].no_divider);
  return not checked or (power_of_two and bits >= 8);
}

void sai::reset(register_file& regs, reset_kind)
{
  for(auto const& name : claims())
  {
    regs.poke(reg(name).offset, describe(name).reset);
  }
  for(auto& block : state_.block)
  {
    auto const generation = block.generation + 1;
    block                 = {};
    block.generation      = generation;
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
  start(b, ctx_.clock.time().ps);
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
    block.stalled = true;
    if(not std::exchange(no_clock_logged_, true))
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
    if((b == source or follows(b, source)) and enabled(b))
    {
      begin(b);
    }
  }
  run(source, 0);
}

auto sai::frame_bits(std::size_t b) const -> std::uint32_t
{
  return get(regs_, block_[b].frame_length) + 1u;
}

auto sai::slot_start(std::size_t b, std::size_t slot, std::uint32_t bits) const -> std::uint32_t
{
  auto const code  = get(regs_, block_[b].slot_size);
  auto const width = code == 1 ? 16u : (code == 2 ? 32u : data_bits[get(regs_, block_[b].size)]);
  auto const count = slots(b);
  auto const index = static_cast<std::uint32_t>(slot);
  if(on(block_[b].fs_definition) and count >= 2)
  {
    auto const half = static_cast<std::uint32_t>(count / 2);
    return index < half ? index * width : bits / 2 + (index - half) * width;
  }
  return index * width;
}

auto sai::slot_end(std::size_t b, std::size_t slot, std::uint32_t bits) const -> std::uint32_t
{
  auto const code  = get(regs_, block_[b].slot_size);
  auto const width = code == 1 ? 16u : (code == 2 ? 32u : data_bits[get(regs_, block_[b].size)]);
  return std::min(slot_start(b, slot, bits) + width, bits);
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
      for(std::size_t slot = 0; slot < slots(b); ++slot)
      {
        auto const fits = slot_start(b, slot, bits) < bits;
        if(not slot_on(b, slot) or not fits)
        {
          continue;
        }
        if(receiver(b) and slot_end(b, slot, bits) == position)
        {
          store(b, slot, source);
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
        finish(b, start_ps);
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
    if(not enabled(source) or r.hz == 0 or too_short(source))
    {
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
    for(std::size_t slot = 0; slot < slots(b); ++slot)
    {
      if(not slot_on(b, slot))
      {
        continue;
      }
      auto const point =
          receiver(b) ? slot_end(b, slot, block.bits) : slot_start(b, slot, block.bits);
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

void sai::store(std::size_t b, std::size_t slot, std::size_t source)
{
  auto& block = state_.block[b];
  if(block.frame.empty())
  {
    block.frame.assign(slots(b), 0);
    if(auto const far_end = wired_[b].endpoint)
    {
      far_end->receive(block.frame, format(b), vtime{time_at(source, 0)});
    }
  }
  if(slot >= block.frame.size() or (slot == 1 and mono(b)))
  {
    return;
  }
  auto const here = static_cast<std::int8_t>(slot);
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
  block.fifo.push_back(block.frame[slot] & mask(b));
}

void sai::finish(std::size_t b, std::uint64_t at_ps)
{
  auto& block        = state_.block[b];
  block.active       = false;
  auto const frame   = std::exchange(block.frame, {});
  auto const far_end = wired_[b].endpoint;
  if(not receiver(b) and far_end)
  {
    far_end->transmitted(frame, format(b), vtime{at_ps});
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
  run(kind, state_.block[kind].position);
  flags();
}

void sai::on_clock_change()
{
  for(std::size_t b = 0; b < blocks; ++b)
  {
    if(state_.block[b].stalled and enabled(b) and frame_rate(b).hz != 0)
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
  wired_[block == audio_block::a ? 0 : 1] = std::move(attachment);
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
