#include "dma_signal.hpp"
#include "factories.hpp"
#include "model_base.hpp"

#include <popcorn/core/clock.hpp>
#include <popcorn/periph/stm32/board.hpp>
#include <popcorn/periph/stm32/gpio.hpp>
#include <popcorn/periph/stm32/spi.hpp>

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
constexpr std::uint64_t ps_per_second = 1'000'000'000'000;
constexpr std::uint32_t frame_done    = 0;
constexpr std::uint32_t full_duplex   = 0;
constexpr std::uint32_t simplex_tx    = 1;
constexpr std::uint32_t simplex_rx    = 2;

struct spi_state
{
  std::uint32_t                       generation{};
  std::vector<std::uint32_t>          tx_fifo;
  std::vector<std::uint32_t>          rx_fifo;
  std::uint32_t                       shift{};
  std::uint64_t                       frame_end_ps{};
  std::uint32_t                       done{};
  std::uint32_t                       written{};
  std::uint8_t                        busy{};
  std::uint8_t                        running{};
  std::uint8_t                        suspending{};
  std::uint8_t                        auto_suspended{};
  std::vector<std::uint8_t>           selected;
  std::vector<std::vector<std::byte>> devices;

  friend auto operator==(spi_state const&, spi_state const&) -> bool = default;
};

auto lower(std::string_view name) -> std::string
{
  std::string out{name};
  std::ranges::transform(
      out, out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

/**
 * @brief SPI1 to SPI3 of the STM32U5, RM0456 section 68 "Serial peripheral interface (SPI)",
 *        controller mode. Frames of CFG1.DSIZE + 1 bits leave one SCK frame time apart, SCK the
 *        kernel clock divided by CFG1.MBR, while CR1.CSTART runs and the TX FIFO holds data, or
 *        continuously in simplex receive. Each frame time is rounded down to a whole picosecond,
 *        an error under 1 ps per frame. A frame keeps the duration it started with across a
 *        clock change, the new rate applies from the next frame. Each frame is exchanged with the
 *        selected devices at its end and lands in the RX FIFO. FIFOs of 16 bytes on SPI1 and
 *        SPI2, 8 on SPI3, a frame taking 1 to 4 bytes, packed by the access size. A 17 to 24-bit
 *        frame takes 3 FIFO bytes, as RM0456 68.4.12 "Data packet control" states. TSIZE frames per
 *        transfer, then EOT, CSTART cleared, and TXTF once TSIZE frames were written, which clears
 *        TXPIE and DXPIE. TSIZE 0 runs until SPE clears. TXC copies EOT when TSIZE is set, and
 *        otherwise is set while the TX FIFO and the shifter are empty. TXP, RXP, DXP by the FTHLV
 *        packet, TXP by the remaining frames for the last packet, RXWNE, RXPLVL, CTSIZE except on
 *        SPI3. A full RX FIFO sets OVR and drops the frame, or with CR1.MASRX suspends the clock
 *        and sets SUSP until IFCR.SUSPC. CSUSP stops after the current frame and sets SUSP. IFCR
 *        clears its flags, re-enabling clears EOT, OVR, UDR and CRCE, the SPI IRQ follows SR and
 *        IER, EOTIE covering EOT, SUSP and TXC. A device is selected by its GPIO chip select pin,
 *        active low, or by the hardware SS output from CSTART to the end of the transfer while
 *        CFG2.SSOE is set. With no device selected MISO reads all ones, a pulled-up line. The
 *        devices are saved and loaded with the model. With CFG1.TXDMAEN the `<instance>_tx_dma`
 *        request follows TXP, with CFG1.RXDMAEN the `<instance>_rx_dma` request follows RXP, or
 *        a partial last packet left in the RX FIFO once EOT is set, RM0456 section 68.4.14,
 *        through the GPDMA peer, both low while SPE is clear. Not modelled, logged once: target
 *        mode, half duplex, CRC, TI mode, the autonomous trigger, the SS pulse and timing
 *        options.
 */
class spi final : public dispatch_model<spi, spi_state>
{
public:
  spi(model_context& ctx, peripheral_desc const& desc, std::vector<spi_attachment> attachments);

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
  void save(std::vector<std::byte>& out) const override;
  void load(std::span<std::byte const> in) override;

  void control_written(std::uint32_t old, std::uint32_t now);
  void clear_written(std::uint32_t old, std::uint32_t now);
  void status_written(std::uint32_t old, std::uint32_t now);
  void plain_written(std::uint32_t old, std::uint32_t now);

private:
  [[nodiscard]] auto frame_bits() const -> std::uint32_t;
  [[nodiscard]] auto frame_bytes() const -> std::uint32_t;
  [[nodiscard]] auto frame_occupancy() const -> std::uint32_t;
  [[nodiscard]] auto frame_mask() const -> std::uint32_t;
  [[nodiscard]] auto packet_frames() const -> std::size_t;
  [[nodiscard]] auto capacity() const -> std::size_t;
  [[nodiscard]] auto transfer_size() const -> std::uint32_t;
  [[nodiscard]] auto communication() const -> std::uint32_t;
  [[nodiscard]] auto frame_ps() -> std::uint64_t;
  [[nodiscard]] auto power_on_offset() const -> std::uint64_t;

  void subscribe();
  void start_frame(std::uint64_t from_ps);
  void finish_frame();
  void stop(bool completed);
  void hardware_select(bool selected);
  void flags();
  void warn_unsupported();

  register_file&                               regs_;
  std::vector<spi_attachment>                  attachments_;
  std::vector<subscription>                    subscriptions_;
  bool                                         subscribed_{};
  bool                                         limited_;
  std::size_t                                  fifo_bytes_;
  reg_ref                                      status_;
  reg_ref                                      clear_;
  reg_ref                                      transmit_;
  reg_ref                                      receive_;
  field_ref                                    enable_;
  field_ref                                    start_;
  field_ref                                    suspend_;
  field_ref                                    master_rx_;
  field_ref                                    size_;
  field_ref                                    data_size_;
  field_ref                                    threshold_;
  field_ref                                    baud_;
  field_ref                                    bypass_;
  field_ref                                    master_;
  field_ref                                    comm_;
  field_ref                                    ss_output_;
  field_ref                                    rx_packet_;
  field_ref                                    tx_packet_;
  field_ref                                    duplex_packet_;
  field_ref                                    end_;
  field_ref                                    filled_;
  field_ref                                    overrun_;
  field_ref                                    suspended_;
  field_ref                                    tx_complete_;
  field_ref                                    rx_level_;
  field_ref                                    rx_word_;
  field_ref                                    tx_packet_ie_;
  field_ref                                    duplex_packet_ie_;
  field_ref                                    remaining_;
  std::vector<std::pair<field_ref, field_ref>> clears_;
  std::vector<std::pair<field_ref, field_ref>> interrupts_;
  std::vector<field_ref>                       unsupported_;
  std::vector<field_ref>                       cleared_on_enable_;
  core::clock_domain                           kernel_;
  irq_line&                                    line_;
  field_ref                                    tx_dma_enable_;
  field_ref                                    rx_dma_enable_;
  dma_signal                                   tx_dma_;
  dma_signal                                   rx_dma_;
  bool                                         unsupported_logged_{};
  bool                                         no_clock_logged_{};
};

constexpr std::array<write_entry<spi>, 8> spi_writes{
    {
     {"SPI_CR1", &spi::control_written},
     {"SPI_CR2", &spi::plain_written},
     {"SPI_CFG1", &spi::plain_written},
     {"SPI_IER", &spi::plain_written},
     {"SPI_SR", &spi::status_written},
     {"SPI_IFCR", &spi::clear_written},
     {"SPI_TXDR", &spi::plain_written},
     {"SPI_RXDR", &spi::plain_written},
     }
};

spi::spi(model_context& ctx, peripheral_desc const& desc, std::vector<spi_attachment> attachments)
    : dispatch_model{ctx, desc, "SPI", spi_writes}, regs_{ctx.regs},
      attachments_{std::move(attachments)}, limited_{desc.name == "SPI3"},
      fifo_bytes_{limited_ ? 8u : 16u}, status_{reg("SPI_SR")}, clear_{reg("SPI_IFCR")},
      transmit_{reg("SPI_TXDR")}, receive_{reg("SPI_RXDR")}, enable_{field("SPI_CR1", "SPE")},
      start_{field("SPI_CR1", "CSTART")}, suspend_{field("SPI_CR1", "CSUSP")},
      master_rx_{field("SPI_CR1", "MASRX")}, size_{field("SPI_CR2", "TSIZE")},
      data_size_{field("SPI_CFG1", "DSIZE")}, threshold_{field("SPI_CFG1", "FTHLV")},
      baud_{field("SPI_CFG1", "MBR")}, bypass_{field("SPI_CFG1", "BPASS")},
      master_{field("SPI_CFG2", "MASTER")}, comm_{field("SPI_CFG2", "COMM")},
      ss_output_{field("SPI_CFG2", "SSOE")}, rx_packet_{field("SPI_SR", "RXP")},
      tx_packet_{field("SPI_SR", "TXP")}, duplex_packet_{field("SPI_SR", "DXP")},
      end_{field("SPI_SR", "EOT")}, filled_{field("SPI_SR", "TXTF")},
      overrun_{field("SPI_SR", "OVR")}, suspended_{field("SPI_SR", "SUSP")},
      tx_complete_{field("SPI_SR", "TXC")}, rx_level_{field("SPI_SR", "RXPLVL")},
      rx_word_{field("SPI_SR", "RXWNE")}, tx_packet_ie_{field("SPI_IER", "TXPIE")},
      duplex_packet_ie_{field("SPI_IER", "DXPIE")}, remaining_{field("SPI_SR", "CTSIZE")},
      kernel_{ctx.clock.domain(lower(desc.name))}, line_{irq(desc.name + "_IRQn")},
      tx_dma_enable_{field("SPI_CFG1", "TXDMAEN")}, rx_dma_enable_{field("SPI_CFG1", "RXDMAEN")},
      tx_dma_{ctx.peers, lower(desc.name) + "_tx_dma"},
      rx_dma_{ctx.peers, lower(desc.name) + "_rx_dma"}
{
  auto const& flags = describe("SPI_SR").fields;
  for(auto const& bits : describe("SPI_IFCR").fields)
  {
    std::string_view const name{bits.name};
    auto const             target = name.substr(0, name.size() - 1);
    if(name.ends_with("C") and std::ranges::contains(flags, target, &field_desc::name))
    {
      clears_.emplace_back(field("SPI_IFCR", name), field("SPI_SR", target));
    }
  }
  for(auto const& bits : describe("SPI_IER").fields)
  {
    std::string_view const name{bits.name};
    auto const             target = name.substr(0, name.size() - 2);
    if(name.ends_with("IE") and std::ranges::contains(flags, target, &field_desc::name))
    {
      interrupts_.emplace_back(field("SPI_IER", name), field("SPI_SR", target));
    }
  }
  auto const eot_ie = field("SPI_IER", "EOTIE");
  interrupts_.emplace_back(eot_ie, suspended_);
  interrupts_.emplace_back(eot_ie, tx_complete_);
  for(auto const name : {"EOT", "OVR", "UDR", "CRCE"})
  {
    cleared_on_enable_.push_back(field("SPI_SR", name));
  }
  for(auto const& [reg_name, name] : std::array<std::pair<std::string_view, std::string_view>, 5>{
          {
           {"SPI_CFG1", "CRCEN"},
           {"SPI_CFG2", "SP"},
           {"SPI_CFG2", "MIDI"},
           {"SPI_CFG2", "MSSI"},
           {"SPI_AUTOCR", "TRIGEN"},
           }
  })
  {
    unsupported_.push_back(field(reg_name, name));
  }
}

auto spi::frame_bits() const -> std::uint32_t
{
  return get(regs_, data_size_) + 1;
}

auto spi::frame_bytes() const -> std::uint32_t
{
  auto const bits = frame_bits();
  return bits <= 8 ? 1 : bits <= 16 ? 2 : 4;
}

auto spi::frame_mask() const -> std::uint32_t
{
  auto const bits = frame_bits();
  return bits >= 32 ? ~std::uint32_t{} : (std::uint32_t{1} << bits) - 1u;
}

auto spi::frame_occupancy() const -> std::uint32_t
{
  return (frame_bits() + 7) / 8;
}

auto spi::packet_frames() const -> std::size_t
{
  return std::size_t{get(regs_, threshold_)} + 1;
}

auto spi::capacity() const -> std::size_t
{
  return std::max<std::size_t>(fifo_bytes_ / frame_occupancy(), 1);
}

auto spi::transfer_size() const -> std::uint32_t
{
  return get(regs_, size_);
}

auto spi::communication() const -> std::uint32_t
{
  return get(regs_, comm_);
}

auto spi::power_on_offset() const -> std::uint64_t
{
  return ctx_.clock.since_power_on().ps - ctx_.clock.time().ps;
}

auto spi::frame_ps() -> std::uint64_t
{
  auto const hz  = ctx_.clock.hz(kernel_);
  auto const div = get(regs_, bypass_) != 0 ? 1u : 2u << get(regs_, baud_);
  if(hz == 0)
  {
    if(not std::exchange(no_clock_logged_, true))
    {
      warn("SPI_CFG1", "the SPI kernel clock is stopped: no frame is transferred");
    }
    return 0;
  }
  return std::uint64_t{frame_bits()} * div * ps_per_second / hz;
}

void spi::subscribe()
{
  if(std::exchange(subscribed_, true))
  {
    return;
  }
  for(std::size_t index = 0; index < attachments_.size(); ++index)
  {
    auto const& attached = attachments_[index];
    if(not attached.cs_pin)
    {
      continue;
    }
    auto* const port = dynamic_cast<gpio_port*>(ctx_.peers.find(attached.cs_port));
    if(port == nullptr)
    {
      throw config_error(
          ctx_.part.part, 0, std::format("board.spi.{}.cs_port", desc_.name),
          std::format("a GPIO port the chip select of a {} device names", desc_.name));
    }
    subscriptions_.push_back(port->on_change([this, index](std::uint8_t pin, bool level) {
      auto const& at = attachments_[index];
      if(pin != *at.cs_pin or index >= state_.selected.size())
      {
        return;
      }
      auto const selected = level ? 0 : 1;
      if(state_.selected[index] != selected)
      {
        state_.selected[index] = static_cast<std::uint8_t>(selected);
        at.device->select(selected != 0, ctx_.clock.since_power_on());
      }
    }));
  }
}

void spi::hardware_select(bool selected)
{
  if(get(regs_, ss_output_) == 0 and selected)
  {
    return;
  }
  auto const at = ctx_.clock.since_power_on();
  for(std::size_t index = 0; index < attachments_.size(); ++index)
  {
    if(attachments_[index].cs_pin or state_.selected[index] == (selected ? 1 : 0))
    {
      continue;
    }
    state_.selected[index] = selected ? 1 : 0;
    attachments_[index].device->select(selected, at);
  }
}

void spi::reset(register_file& regs, reset_kind kind)
{
  for(auto const& name : claims())
  {
    regs.poke(reg(name).offset, describe(name).reset);
  }
  auto const generation = state_.generation + 1;
  auto       selected   = state_.selected;
  state_                = {};
  state_.generation     = generation;
  selected.resize(attachments_.size());
  state_.selected = selected;
  for(auto const& attached : attachments_)
  {
    attached.device->reset(kind);
  }
  for(std::size_t index = 0; index < attachments_.size(); ++index)
  {
    if(not attachments_[index].cs_pin and state_.selected[index] != 0)
    {
      state_.selected[index] = 0;
      attachments_[index].device->select(false, ctx_.clock.since_power_on());
    }
  }
  subscribe();
  tx_dma_.resolve();
  rx_dma_.resolve();
  flags();
}

void spi::start_frame(std::uint64_t from_ps)
{
  if(state_.busy != 0 or state_.running == 0 or state_.auto_suspended != 0)
  {
    return;
  }
  auto const size = transfer_size();
  if(size != 0 and state_.done >= size)
  {
    return;
  }
  auto const receive_only = communication() == simplex_rx;
  if(not receive_only and state_.tx_fifo.empty())
  {
    return;
  }
  auto const rx_full = communication() != simplex_tx and state_.rx_fifo.size() >= capacity();
  if(rx_full and get(regs_, master_rx_) != 0)
  {
    state_.auto_suspended = 1;
    update(regs_, suspended_, 1);
    return;
  }
  auto const ps = frame_ps();
  if(ps == 0)
  {
    return;
  }
  if(receive_only)
  {
    state_.shift = frame_mask();
  }
  else
  {
    state_.shift = state_.tx_fifo.front();
    state_.tx_fifo.erase(state_.tx_fifo.begin());
  }
  state_.busy         = 1;
  state_.frame_end_ps = std::max(from_ps + ps, ctx_.clock.time().ps);
  ctx_.events.schedule_at(vtime{state_.frame_end_ps}, frame_done, ++state_.generation, ctx_.clock);
}

void spi::finish_frame()
{
  state_.busy   = 0;
  auto const at = vtime{state_.frame_end_ps + power_on_offset()};
  auto       in = frame_mask();
  for(std::size_t index = 0; index < attachments_.size(); ++index)
  {
    if(state_.selected[index] != 0)
    {
      in &= attachments_[index].device->exchange(
          state_.shift, static_cast<std::uint8_t>(frame_bits()), at);
    }
  }
  if(communication() != simplex_tx)
  {
    if(state_.rx_fifo.size() < capacity())
    {
      state_.rx_fifo.push_back(in & frame_mask());
    }
    else
    {
      update(regs_, overrun_, 1);
    }
  }
  ++state_.done;
  auto const size = transfer_size();
  if(size != 0 and state_.done >= size)
  {
    stop(true);
    return;
  }
  if(state_.suspending != 0)
  {
    state_.suspending = 0;
    update(regs_, suspended_, 1);
    stop(false);
    return;
  }
  start_frame(state_.frame_end_ps);
}

void spi::stop(bool completed)
{
  state_.running        = 0;
  state_.auto_suspended = 0;
  update(regs_, start_, 0);
  if(completed)
  {
    update(regs_, end_, 1);
  }
  hardware_select(false);
}

void spi::flags()
{
  auto const size    = transfer_size();
  auto const all_in  = size != 0 and state_.written >= size;
  auto const left    = size != 0 and not all_in ? std::size_t{size - state_.written} : capacity();
  auto const packet  = packet_frames();
  auto const space   = capacity() - std::min(capacity(), state_.tx_fifo.size());
  auto const tx_room = space >= std::min(packet, left) and not all_in;
  auto const rx_full = state_.rx_fifo.size() >= packet;
  update(regs_, tx_packet_, tx_room ? 1 : 0);
  update(regs_, rx_packet_, rx_full ? 1 : 0);
  update(regs_, duplex_packet_, tx_room and rx_full ? 1 : 0);
  if(all_in and get(regs_, filled_) == 0)
  {
    update(regs_, filled_, 1);
    update(regs_, tx_packet_ie_, 0);
    update(regs_, duplex_packet_ie_, 0);
  }
  auto const idle = state_.tx_fifo.empty() and state_.busy == 0;
  auto const done = get(regs_, enable_) == 0 or (size != 0 ? get(regs_, end_) != 0 : idle);
  update(regs_, tx_complete_, done ? 1 : 0);
  auto const bytes = state_.rx_fifo.size() * frame_occupancy();
  update(regs_, rx_word_, bytes >= 4 ? 1 : 0);
  update(
      regs_, rx_level_,
      frame_bits() <= 16 ? static_cast<std::uint32_t>(bytes % 4 / frame_occupancy()) : 0);
  update(regs_, remaining_, not limited_ and size > state_.done ? size - state_.done : 0);
  regs_.poke(receive_.offset, state_.rx_fifo.empty() ? 0 : state_.rx_fifo.front());
  auto level = false;
  for(auto const& [enable, flag] : interrupts_)
  {
    level = level or (get(regs_, enable) != 0 and get(regs_, flag) != 0);
  }
  if(level)
  {
    line_.raise();
  }
  else
  {
    line_.lower();
  }
  auto const up        = get(regs_, enable_) != 0;
  auto const remainder = get(regs_, end_) != 0 and not state_.rx_fifo.empty();
  tx_dma_.set(up and get(regs_, tx_dma_enable_) != 0 and tx_room);
  rx_dma_.set(up and get(regs_, rx_dma_enable_) != 0 and (rx_full or remainder));
}

void spi::warn_unsupported()
{
  auto const used =
      get(regs_, master_)
      == 0
      or communication()
      == 3
      or std::ranges::any_of(unsupported_, [&](field_ref bits) { return get(regs_, bits) != 0; });
  if(used and not std::exchange(unsupported_logged_, true))
  {
    warn(
        "SPI_CFG2",
        "target mode, half duplex, CRC, TI mode, the autonomous trigger or SS timing is "
        "configured: not modelled, the SPI runs as a plain controller");
  }
}

void spi::control_written(std::uint32_t old, std::uint32_t now)
{
  if(enable_.of(old) != 0 and enable_.of(now) == 0)
  {
    ++state_.generation;
    state_.tx_fifo.clear();
    state_.rx_fifo.clear();
    state_.busy       = 0;
    state_.suspending = 0;
    state_.done       = 0;
    state_.written    = 0;
    stop(false);
    return;
  }
  if(enable_.of(now) == 0)
  {
    update(regs_, start_, 0);
    return;
  }
  if(enable_.of(old) == 0)
  {
    for(auto const flag : cleared_on_enable_)
    {
      update(regs_, flag, 0);
    }
  }
  if(suspend_.of(now) != 0)
  {
    update(regs_, suspend_, 0);
    if(state_.running != 0)
    {
      state_.suspending = 1;
      if(state_.busy == 0)
      {
        state_.suspending = 0;
        update(regs_, suspended_, 1);
        stop(false);
      }
    }
  }
  if(start_.of(old) == 0 and start_.of(now) != 0)
  {
    warn_unsupported();
    if(get(regs_, master_) == 0)
    {
      return;
    }
    state_.running = 1;
    state_.done    = 0;
    hardware_select(true);
    start_frame(ctx_.clock.time().ps);
  }
}

void spi::clear_written(std::uint32_t, std::uint32_t now)
{
  for(auto const& [clear, flag] : clears_)
  {
    if(clear.of(now) != 0)
    {
      update(regs_, flag, 0);
      if(flag.lsb == filled_.lsb)
      {
        state_.written = 0;
      }
    }
  }
  regs_.poke(clear_.offset, 0);
  if(get(regs_, suspended_) == 0 and std::exchange(state_.auto_suspended, 0) != 0)
  {
    start_frame(ctx_.clock.time().ps);
  }
}

void spi::status_written(std::uint32_t old, std::uint32_t)
{
  regs_.poke(status_.offset, old);
}

void spi::plain_written(std::uint32_t, std::uint32_t)
{}

auto spi::on_read(register_file& regs, std::uint32_t offset, std::uint8_t size, access_ctx access)
    -> std::expected<std::uint32_t, bus_error>
{
  if((offset & ~3u) != receive_.offset or access.by == initiator::debugger)
  {
    return dispatch_model::on_read(regs, offset, size, access);
  }
  auto const bytes = frame_bytes();
  auto const count = std::max<std::uint32_t>(size / bytes, 1);
  auto       value = std::uint32_t{};
  for(std::uint32_t index = 0; index < count and not state_.rx_fifo.empty(); ++index)
  {
    value |= state_.rx_fifo.front() << (8 * bytes * index);
    state_.rx_fifo.erase(state_.rx_fifo.begin());
  }
  start_frame(ctx_.clock.time().ps);
  flags();
  return size >= 4 ? value : value & ((std::uint32_t{1} << (8u * size)) - 1u);
}

auto spi::on_write(
    register_file& regs,
    std::uint32_t  offset,
    std::uint8_t   size,
    std::uint32_t  old,
    std::uint32_t  value,
    access_ctx     access) -> std::expected<void, bus_error>
{
  if((offset & ~3u) == transmit_.offset)
  {
    auto const bytes = frame_bytes();
    auto const count = std::max<std::uint32_t>(size / bytes, 1);
    auto const limit = transfer_size();
    for(std::uint32_t index = 0; index < count; ++index)
    {
      auto const wanted = limit == 0 or state_.written < limit;
      if(wanted and state_.tx_fifo.size() < capacity() and get(regs_, enable_) != 0)
      {
        state_.tx_fifo.push_back((value >> (8 * bytes * index)) & frame_mask());
        ++state_.written;
      }
    }
    regs_.poke(transmit_.offset, 0);
    start_frame(ctx_.clock.time().ps);
    flags();
    return {};
  }
  auto const result = dispatch_model::on_write(regs, offset, size, old, value, access);
  flags();
  return result;
}

void spi::on_event(std::uint32_t kind, std::uint64_t payload)
{
  if(kind != frame_done or payload != state_.generation or state_.busy == 0)
  {
    return;
  }
  finish_frame();
  flags();
}

void spi::save(std::vector<std::byte>& out) const
{
  auto copy = state_;
  copy.devices.clear();
  for(auto const& attached : attachments_)
  {
    attached.device->save(copy.devices.emplace_back());
  }
  reflex::serde::bson::serializer{out}.dump(copy);
}

void spi::load(std::span<std::byte const> in)
{
  state_ = reflex::serde::bson::deserializer{in}.template load<spi_state>();
  for(std::size_t index = 0; index < attachments_.size() and index < state_.devices.size(); ++index)
  {
    attachments_[index].device->load(state_.devices[index]);
  }
  state_.devices.clear();
  state_.selected.resize(attachments_.size());
  subscribe();
}
}

auto spi_factory(board const& b) -> model_factory
{
  return
      [devices = b.spi](model_context& ctx, peripheral_desc const& desc) -> std::shared_ptr<model> {
        auto const found = devices.find(desc.name);
        return std::make_shared<spi>(
            ctx, desc, found != devices.end() ? found->second : std::vector<spi_attachment>{});
      };
}
}
