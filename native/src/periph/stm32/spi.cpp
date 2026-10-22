#include "dma_signal.hpp"
#include "factories.hpp"
#include "model_base.hpp"

#include <popcorn/core/clock.hpp>
#include <popcorn/core/scheduler.hpp>
#include <popcorn/periph/spi.hpp>
#include <popcorn/periph/stm32/board.hpp>
#include <popcorn/periph/stm32/gpio.hpp>
#include <popcorn/periph/stm32/spi.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <format>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
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
constexpr std::uint32_t host_edge     = 1;
constexpr std::uint32_t full_duplex   = 0;
constexpr std::uint32_t simplex_tx    = 1;
constexpr std::uint32_t simplex_rx    = 2;

struct spi_host_transfer
{
  std::uint64_t              start_ps{};
  std::uint64_t              frame_ps{};
  std::uint8_t               bits{};
  std::vector<std::uint32_t> frames;

  friend auto operator==(spi_host_transfer const&, spi_host_transfer const&) -> bool = default;
};

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
  std::vector<spi_host_transfer>      host;
  std::uint32_t                       host_generation{};
  std::uint32_t                       host_next{};
  std::uint32_t                       host_shift{};
  std::uint32_t                       last_received{};
  std::uint8_t                        host_busy{};
  std::uint8_t                        host_taking{};
  std::uint8_t                        host_selected{};
  std::uint8_t                        host_synced{};
  std::vector<std::uint8_t>           mode_checked;
  std::uint8_t                        host_mode_checked{};

  friend auto operator==(spi_state const&, spi_state const&) -> bool = default;
};

constexpr auto mask_of(std::uint32_t bits) -> std::uint32_t
{
  return bits >= 32 ? ~std::uint32_t{} : (std::uint32_t{1} << bits) - 1u;
}

constexpr auto reversed(std::uint32_t value, std::uint32_t bits) -> std::uint32_t
{
  auto out = std::uint32_t{};
  for(std::uint32_t bit = 0; bit < bits; ++bit)
  {
    out = out << 1 | (value >> bit & 1u);
  }
  return out;
}

static_assert(reversed(0x01, 8) == 0x80);
static_assert(reversed(0x001, 12) == 0x800);
static_assert(reversed(0x1234'5678, 32) == 0x1E6A'2C48);

struct heard_frame
{
  std::uint32_t sent;
  std::uint32_t received;
  std::uint64_t at_ps;
};

auto pin_name(spi_attachment const& attached) -> std::string
{
  if(not attached.cs_pin or attached.cs_port.empty())
  {
    return "SS";
  }
  return std::format("P{}{}", attached.cs_port.back(), *attached.cs_pin);
}

auto lower(std::string_view name) -> std::string
{
  std::string out{name};
  std::ranges::transform(
      out, out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

/**
 * @brief SPI1 to SPI3 of the STM32U5, RM0456 section 68 "Serial peripheral interface (SPI)",
 *        controller and target mode. Controller mode, CFG2.MASTER 1: frames of CFG1.DSIZE + 1 bits
 *        leave one SCK frame time apart, SCK the kernel clock divided by CFG1.MBR, while CR1.CSTART
 *        runs and the TX FIFO holds data, or continuously in simplex receive. Each frame time is
 *        rounded down to a whole picosecond, an error under 1 ps per frame. A frame keeps the
 *        duration it started with across a clock change, the new rate applies from the next frame.
 *        Each frame is exchanged with the selected devices at its end and lands in the RX FIFO.
 *        Frames cross the bus in wire order, the first bit on the wire in bit DSIZE: the register
 *        value with CFG2.LSBFRST 0, the value bit reversed over DSIZE + 1 bits with LSBFRST 1,
 *        RM0456 68.4.9 "Data frame format", both ways and in both modes. A device or controller
 *        declaring an SPI mode, spi_target::mode() or spi_controller_sink::mode(), is checked
 *        against CFG2.CPOL << 1 | CFG2.CPHA once per transfer: a device at the end of the first
 *        frame it exchanges after a select edge or CSTART, the controller at the start of the
 *        first frame of each of its transfers the target takes part in. A mismatch fails the run
 *        with config_error naming both modes, and every mismatching device of that frame, once
 *        the model has handled that frame event: for a device after the frame it exchanged, for
 *        the controller at the start of the frame, before its end. The model stays consistent
 *        and a resumed run is not refused again for that transfer. Which checks a transfer has
 *        run is saved with the model. A device declaring none is never checked.
 *        FIFOs of 16 bytes on SPI1 and SPI2, 8 on SPI3, a frame taking 1 to 4 bytes, packed by the
 *        access size. A 17 to 24-bit frame takes 3 FIFO bytes, as RM0456 68.4.12 "Data packet
 *        control" states. TSIZE frames per transfer, then EOT, CSTART cleared, and TXTF once TSIZE
 *        frames were written, which clears TXPIE and DXPIE. TSIZE 0 runs until SPE clears. TXC
 *        copies EOT when TSIZE is set, and otherwise is set while the TX FIFO and the shifter are
 *        empty. TXP, RXP, DXP by the FTHLV packet, TXP by the remaining frames for the last packet,
 *        RXWNE, RXPLVL, CTSIZE except on SPI3. A full RX FIFO sets OVR and drops the frame, or with
 *        CR1.MASRX suspends the clock and sets SUSP until IFCR.SUSPC. CSUSP stops after the current
 *        frame and sets SUSP. IFCR clears its flags, re-enabling clears EOT, OVR, UDR and CRCE, the
 *        SPI IRQ follows SR and IER while SPE is set, EOTIE covering EOT, SUSP and TXC, and only
 *        MODF with MODFIE while SPE is clear, RM0456 section 68.7. A device is selected by its GPIO
 *        chip select pin, active low, or by the hardware SS output from CSTART to the end of the
 *        transfer while CFG2.SSOE is set. With no device selected MISO reads all ones, a pulled-up
 *        line. The devices are saved and loaded with the model. With CFG1.TXDMAEN the
 *        `<instance>_tx_dma` request follows TXP, with CFG1.RXDMAEN the `<instance>_rx_dma` request
 *        follows RXP, or a partial last packet left in the RX FIFO once EOT is set, RM0456 section
 *        68.4.14, through the GPDMA peer, both low while SPE is clear.
 *
 *        Target mode, MASTER 0: the spi_controller_sink of the instance clocks it through
 *        spi_controller_port::transfer, one queued transfer after the other, each frame time fixed
 *        by the controller. The frame events go on core::host_lane, after the model's own events
 *        of the same instant. The NSS input is the controller's select, not a GPIO pin, with
 *        CFG2.SSM 0, deviation, and CR1.SSI compared with CFG2.SSIOP with SSM 1, RM0456 68.4.7.
 *        With SSM 0 a transfer selected while SPE is clear or MASTER is set is ignored to its end,
 *        as RM0456 68.4.11 ignores transfers until an SS edge seen while enabled. A frame the
 *        target takes part in, SPE 1, MASTER 0 and NSS active, loads its output at its start:
 *        the TX FIFO head or, with the FIFO empty or SR.UDR still set, an underrun that sets UDR
 *        and sends SPI_UDRDR with CFG1.UDRCFG 0, the last frame received with UDRCFG 1, all zeros
 *        then in simplex transmit, RM0456 68.5.2. Deviation: the one dummy frame RM0456 Figure 858
 *        shows ahead of the UDRDR pattern is UDRDR too, and UDR rises at once, without the
 *        propagation latency. UDR holds the pattern until IFCR.UDRC or a re-enable. Simplex
 *        receive leaves MISO undriven, all ones. At its end the frame lands in the RX FIFO, or
 *        sets OVR when full, except in simplex transmit, TSIZE counts it, EOT and TXC follow as
 *        in controller mode, and the target ignores frames once TSIZE frames were counted, until
 *        SPE is cleared, an emulator choice. A frame of a size other than DSIZE + 1 is shifted with
 *        the controller's size and masked to DSIZE + 1, logged once. The controller hears each
 *        frame at its end with what the target drove, all ones for a frame it ignored, masked to
 *        the controller's size. A controller-mode SPI selected by the controller with SSM 0 and
 *        SSOE 0 takes a mode fault, RM0456 68.5.2: MODF set, SPE and MASTER cleared, the transfer
 *        ignored, SPE kept clear while MODF is set. Setting SPE with MASTER 1, SSM 0 and SSOE 0
 *        while a controller transfer selects the SPI takes the same fault, SPE staying clear.
 *        With SSOE 1 it ignores the transfer, RM0456 68.4.7, and the SSI input of SSM 1 raises no
 *        MODF, not modelled. Clearing SPE during a frame drops that frame: it neither lands nor
 *        counts. The controller attached to an instance is its only device. The queue is saved
 *        with the model, a reset drops it.
 *
 *        Not modelled, logged once: half duplex, CRC, TI mode, the autonomous trigger, the SS pulse
 *        and timing options.
 */
class spi final : public dispatch_model<spi, spi_state>,
                  public spi_controller_port,
                  public bus_reset_listener
{
public:
  spi(model_context&                       ctx,
      peripheral_desc const&               desc,
      std::vector<spi_attachment>          attachments,
      std::shared_ptr<spi_controller_sink> controller);

  void reset(register_file& regs, reset_kind kind) override;
  void on_bus_reset() override;
  void power_cut() override;
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
  auto transfer(
      vtime                          start,
      std::uint64_t                  frame_ps,
      std::uint8_t                   bits,
      std::span<std::uint32_t const> frames) -> vtime override;

  void control_written(std::uint32_t old, std::uint32_t now);
  void clear_written(std::uint32_t old, std::uint32_t now);
  void status_written(std::uint32_t old, std::uint32_t now);
  void plain_written(std::uint32_t old, std::uint32_t now);

private:
  void               restore_reset_values(std::optional<reset_kind> kind);
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
  [[nodiscard]] auto wire_frame(std::uint32_t value) const -> std::uint32_t;
  [[nodiscard]] auto bus_mode() const -> std::uint8_t;
  void               check_mode(std::optional<std::uint8_t> declared, auto const& who);
  void               check_device(std::size_t index);
  void               rethrow_refused();

  void               subscribe();
  void               start_frame(std::uint64_t from_ps);
  void               finish_frame();
  void               stop(bool completed);
  void               hardware_select(bool selected);
  void               flags();
  void               warn_unsupported();
  void               disable();
  void               host_edge_reached();
  void               begin_host_frame();
  void               end_host_frame();
  [[nodiscard]] auto host_selects() const -> bool;
  [[nodiscard]] auto faults_on_select() const -> bool;

  register_file&                               regs_;
  std::vector<spi_attachment>                  attachments_;
  std::shared_ptr<spi_controller_sink>         controller_;
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
  std::vector<std::pair<field_ref, field_ref>> mode_fault_;
  std::vector<field_ref>                       unsupported_;
  std::vector<field_ref>                       cleared_on_enable_;
  core::clock_domain                           kernel_;
  irq_line&                                    line_;
  field_ref                                    tx_dma_enable_;
  field_ref                                    rx_dma_enable_;
  dma_signal                                   tx_dma_;
  dma_signal                                   rx_dma_;
  field_ref                                    mode_fault_flag_;
  field_ref                                    software_select_;
  field_ref                                    select_input_;
  field_ref                                    select_polarity_;
  field_ref                                    underrun_;
  field_ref                                    underrun_config_;
  field_ref                                    underrun_data_;
  field_ref                                    lsb_first_;
  field_ref                                    polarity_;
  field_ref                                    phase_;
  std::vector<std::string>                     mismatched_;
  bool                                         unsupported_logged_{};
  bool                                         no_clock_logged_{};
  bool                                         size_logged_{};
};

constexpr std::array<write_entry<spi>, 9> spi_writes{
    {
     {"SPI_CR1", &spi::control_written},
     {"SPI_CR2", &spi::plain_written},
     {"SPI_CFG1", &spi::plain_written},
     {"SPI_IER", &spi::plain_written},
     {"SPI_SR", &spi::status_written},
     {"SPI_IFCR", &spi::clear_written},
     {"SPI_TXDR", &spi::plain_written},
     {"SPI_RXDR", &spi::plain_written},
     {"SPI_UDRDR", &spi::plain_written},
     }
};

spi::spi(
    model_context&                       ctx,
    peripheral_desc const&               desc,
    std::vector<spi_attachment>          attachments,
    std::shared_ptr<spi_controller_sink> controller)
    : dispatch_model{ctx, desc, "SPI", spi_writes}, regs_{ctx.regs},
      attachments_{std::move(attachments)}, controller_{std::move(controller)},
      limited_{desc.name == "SPI3"}, fifo_bytes_{limited_ ? 8u : 16u}, status_{reg("SPI_SR")},
      clear_{reg("SPI_IFCR")}, transmit_{reg("SPI_TXDR")}, receive_{reg("SPI_RXDR")},
      enable_{field("SPI_CR1", "SPE")}, start_{field("SPI_CR1", "CSTART")},
      suspend_{field("SPI_CR1", "CSUSP")}, master_rx_{field("SPI_CR1", "MASRX")},
      size_{field("SPI_CR2", "TSIZE")}, data_size_{field("SPI_CFG1", "DSIZE")},
      threshold_{field("SPI_CFG1", "FTHLV")}, baud_{field("SPI_CFG1", "MBR")},
      bypass_{field("SPI_CFG1", "BPASS")}, master_{field("SPI_CFG2", "MASTER")},
      comm_{field("SPI_CFG2", "COMM")}, ss_output_{field("SPI_CFG2", "SSOE")},
      rx_packet_{field("SPI_SR", "RXP")}, tx_packet_{field("SPI_SR", "TXP")},
      duplex_packet_{field("SPI_SR", "DXP")}, end_{field("SPI_SR", "EOT")},
      filled_{field("SPI_SR", "TXTF")}, overrun_{field("SPI_SR", "OVR")},
      suspended_{field("SPI_SR", "SUSP")}, tx_complete_{field("SPI_SR", "TXC")},
      rx_level_{field("SPI_SR", "RXPLVL")}, rx_word_{field("SPI_SR", "RXWNE")},
      tx_packet_ie_{field("SPI_IER", "TXPIE")}, duplex_packet_ie_{field("SPI_IER", "DXPIE")},
      remaining_{field("SPI_SR", "CTSIZE")}, kernel_{ctx.clock.domain(lower(desc.name))},
      line_{irq(desc.name + "_IRQn")}, tx_dma_enable_{field("SPI_CFG1", "TXDMAEN")},
      rx_dma_enable_{field("SPI_CFG1", "RXDMAEN")},
      tx_dma_{ctx.peers, lower(desc.name) + "_tx_dma"},
      rx_dma_{ctx.peers, lower(desc.name) + "_rx_dma"}, mode_fault_flag_{field("SPI_SR", "MODF")},
      software_select_{field("SPI_CFG2", "SSM")}, select_input_{field("SPI_CR1", "SSI")},
      select_polarity_{field("SPI_CFG2", "SSIOP")}, underrun_{field("SPI_SR", "UDR")},
      underrun_config_{field("SPI_CFG1", "UDRCFG")}, underrun_data_{field("SPI_UDRDR", "UDRDR")},
      lsb_first_{field("SPI_CFG2", "LSBFRST")}, polarity_{field("SPI_CFG2", "CPOL")},
      phase_{field("SPI_CFG2", "CPHA")}
{
  if(controller_ and not attachments_.empty())
  {
    throw config_error(
        ctx.part.part, 0, std::format("board.spi.{}", desc.name),
        "an SPI controller alone on the instance, no device next to it");
  }
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
    if(not name.ends_with("IE") or not std::ranges::contains(flags, target, &field_desc::name))
    {
      continue;
    }
    if(target == "MODF")
    {
      mode_fault_.emplace_back(field("SPI_IER", name), field("SPI_SR", target));
    }
    else
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
  return mask_of(frame_bits());
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

auto spi::wire_frame(std::uint32_t value) const -> std::uint32_t
{
  auto const frame = value & frame_mask();
  return get(regs_, lsb_first_) != 0 ? reversed(frame, frame_bits()) : frame;
}

auto spi::bus_mode() const -> std::uint8_t
{
  return static_cast<std::uint8_t>(get(regs_, polarity_) << 1 | get(regs_, phase_));
}

void spi::check_mode(std::optional<std::uint8_t> declared, auto const& who)
{
  if(declared and *declared != bus_mode())
  {
    mismatched_.push_back(std::format("the SPI mode {} {} declares", *declared, who()));
  }
}

void spi::check_device(std::size_t index)
{
  if(std::exchange(state_.mode_checked[index], 1) != 0)
  {
    return;
  }
  auto const& attached = attachments_[index];
  check_mode(
      attached.device->mode(), [&] { return std::format("the device on {}", pin_name(attached)); });
}

void spi::rethrow_refused()
{
  if(mismatched_.empty())
  {
    return;
  }
  auto declared = std::exchange(mismatched_, {});
  auto joined   = std::string{};
  for(auto const& one : declared)
  {
    joined += joined.empty() ? one : " and " + one;
  }
  throw config_error(
      ctx_.part.part, 0, std::format("devices.{}", desc_.name),
      std::format(
          "{}, not mode {} the firmware set, CPOL {} CPHA {}", joined, bus_mode(),
          get(regs_, polarity_), get(regs_, phase_)));
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
        state_.selected[index]     = static_cast<std::uint8_t>(selected);
        state_.mode_checked[index] = 0;
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

void spi::reset(register_file&, reset_kind kind)
{
  restore_reset_values(kind);
}

void spi::on_bus_reset()
{
  restore_reset_values(std::nullopt);
}

void spi::restore_reset_values(std::optional<reset_kind> kind)
{
  for(auto const& name : claims())
  {
    regs_.poke(reg(name).offset, describe(name).reset);
  }
  auto const generation      = state_.generation + 1;
  auto const host_generation = state_.host_generation + (state_.host.empty() ? 0u : 1u);
  auto       selected        = state_.selected;
  state_                     = {};
  state_.generation          = generation;
  state_.host_generation     = host_generation;
  selected.resize(attachments_.size());
  state_.selected = selected;
  state_.mode_checked.resize(attachments_.size());
  if(kind)
  {
    for(auto const& attached : attachments_)
    {
      attached.device->reset(*kind);
    }
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

void spi::power_cut()
{
  auto const         at = ctx_.clock.since_power_on();
  std::exception_ptr first;
  for(auto const& attached : attachments_)
  {
    try
    {
      attached.device->power_cut(at);
    }
    catch(...)
    {
      if(not first)
      {
        first = std::current_exception();
      }
    }
  }
  if(first)
  {
    std::rethrow_exception(first);
  }
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
  state_.busy    = 0;
  auto const at  = vtime{state_.frame_end_ps + power_on_offset()};
  auto const out = wire_frame(state_.shift);
  auto       in  = frame_mask();
  for(std::size_t index = 0; index < attachments_.size(); ++index)
  {
    if(state_.selected[index] != 0)
    {
      check_device(index);
      in &= attachments_[index].device->exchange(out, static_cast<std::uint8_t>(frame_bits()), at);
    }
  }
  if(communication() != simplex_tx)
  {
    if(state_.rx_fifo.size() < capacity())
    {
      state_.rx_fifo.push_back(wire_frame(in));
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
  auto const idle = state_.tx_fifo.empty()
                and state_.busy == 0
                and (state_.host_busy == 0 or state_.host_taking == 0);
  auto const done = get(regs_, enable_) == 0 or (size != 0 ? get(regs_, end_) != 0 : idle);
  update(regs_, tx_complete_, done ? 1 : 0);
  auto const bytes = state_.rx_fifo.size() * frame_occupancy();
  update(regs_, rx_word_, bytes >= 4 ? 1 : 0);
  update(
      regs_, rx_level_,
      frame_bits() <= 16 ? static_cast<std::uint32_t>(bytes % 4 / frame_occupancy()) : 0);
  update(regs_, remaining_, not limited_ and size > state_.done ? size - state_.done : 0);
  regs_.poke(receive_.offset, state_.rx_fifo.empty() ? 0 : state_.rx_fifo.front());
  auto const up    = get(regs_, enable_) != 0;
  auto       level = false;
  for(auto const& [enable, flag] : mode_fault_)
  {
    level = level or (get(regs_, enable) != 0 and get(regs_, flag) != 0);
  }
  for(auto const& [enable, flag] : interrupts_)
  {
    level = level or (up and get(regs_, enable) != 0 and get(regs_, flag) != 0);
  }
  if(level)
  {
    line_.raise();
  }
  else
  {
    line_.lower();
  }
  auto const remainder = get(regs_, end_) != 0 and not state_.rx_fifo.empty();
  tx_dma_.set(up and get(regs_, tx_dma_enable_) != 0 and tx_room);
  rx_dma_.set(up and get(regs_, rx_dma_enable_) != 0 and (rx_full or remainder));
}

void spi::warn_unsupported()
{
  auto const used = communication() == 3 or std::ranges::any_of(unsupported_, [&](field_ref bits) {
                      return get(regs_, bits) != 0;
                    });
  if(used and not std::exchange(unsupported_logged_, true))
  {
    warn(
        "SPI_CFG2",
        "half duplex, CRC, TI mode, the autonomous trigger or SS timing is configured: not "
        "modelled, the SPI runs as a plain full-duplex or simplex SPI");
  }
}

void spi::disable()
{
  ++state_.generation;
  state_.tx_fifo.clear();
  state_.rx_fifo.clear();
  state_.busy        = 0;
  state_.suspending  = 0;
  state_.done        = 0;
  state_.written     = 0;
  state_.host_synced = 0;
  state_.host_taking = 0;
  state_.host_shift  = ~std::uint32_t{};
  stop(false);
}

void spi::control_written(std::uint32_t old, std::uint32_t now)
{
  if(enable_.of(old) != 0 and enable_.of(now) == 0)
  {
    disable();
    return;
  }
  if(enable_.of(old) == 0 and enable_.of(now) != 0)
  {
    if(state_.host_selected != 0 and faults_on_select())
    {
      update(regs_, mode_fault_flag_, 1);
      update(regs_, master_, 0);
    }
    if(get(regs_, mode_fault_flag_) != 0)
    {
      update(regs_, enable_, 0);
      update(regs_, start_, 0);
      return;
    }
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
    std::ranges::fill(state_.mode_checked, 0);
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
  if(kind == host_edge)
  {
    if(payload == state_.host_generation)
    {
      host_edge_reached();
      rethrow_refused();
    }
    return;
  }
  if(kind != frame_done or payload != state_.generation or state_.busy == 0)
  {
    return;
  }
  finish_frame();
  flags();
  rethrow_refused();
}

auto spi::transfer(
    vtime                          start,
    std::uint64_t                  frame_ps,
    std::uint8_t                   bits,
    std::span<std::uint32_t const> frames) -> vtime
{
  if(not controller_)
  {
    throw config_error(
        ctx_.part.part, 0, std::format("devices.{}", desc_.name),
        "an SPI controller attached to the instance");
  }
  if(frame_ps == 0 or bits < 4 or bits > 32 or frames.empty() or frames.size() > max_frames)
  {
    throw std::invalid_argument(
        "an SPI transfer has 1 to 4096 frames of 4 to 32 bits, each at least 1 ps long");
  }
  if(std::ranges::any_of(
         frames, [&](std::uint32_t frame) { return (frame & ~mask_of(bits)) != 0; }))
  {
    throw std::invalid_argument(std::format("an SPI frame of {} bits is below 2**{}", bits, bits));
  }
  auto const now = ctx_.clock.time().ps;
  if(start.ps < now)
  {
    throw std::invalid_argument(
        std::format(
            "an SPI transfer at {} ps starts before the machine time {} ps", start.ps, now));
  }
  auto queued = std::size_t{};
  auto from   = start.ps;
  for(auto const& queued_transfer : state_.host)
  {
    queued += queued_transfer.frames.size();
    from = std::max(
        from, queued_transfer.start_ps + queued_transfer.frames.size() * queued_transfer.frame_ps);
  }
  if(queued + frames.size() > max_frames)
  {
    throw config_error(
        ctx_.part.part, 0, std::format("devices.{}", desc_.name),
        std::format(
            "at most {} SPI frames queued, {} are and the transfer adds {}", max_frames, queued,
            frames.size()));
  }
  auto constexpr last = std::numeric_limits<std::uint64_t>::max();
  if(frame_ps > (last - from) / frames.size())
  {
    throw std::invalid_argument("an SPI transfer ends within the 64-bit picosecond range");
  }
  auto const idle = state_.host.empty();
  state_.host.push_back({
      .start_ps = from,
      .frame_ps = frame_ps,
      .bits     = bits,
      .frames   = {frames.begin(), frames.end()}
  });
  if(idle)
  {
    ctx_.events.schedule_at(
        vtime{from}, host_edge, state_.host_generation, core::host_lane, ctx_.clock);
  }
  return vtime{from};
}

auto spi::faults_on_select() const -> bool
{
  return get(regs_, master_) != 0
     and get(regs_, software_select_) == 0
     and get(regs_, ss_output_) == 0;
}

auto spi::host_selects() const -> bool
{
  if(get(regs_, enable_) == 0 or get(regs_, master_) != 0)
  {
    return false;
  }
  if(get(regs_, software_select_) != 0)
  {
    return get(regs_, select_input_) == get(regs_, select_polarity_);
  }
  return state_.host_selected != 0 and state_.host_synced != 0;
}

void spi::begin_host_frame()
{
  state_.host_busy   = 1;
  state_.host_taking = 0;
  state_.host_shift  = ~std::uint32_t{};
  auto const size    = transfer_size();
  if(not host_selects() or (size != 0 and state_.done >= size))
  {
    return;
  }
  warn_unsupported();
  state_.host_taking = 1;
  if(std::exchange(state_.host_mode_checked, 1) == 0)
  {
    check_mode(controller_->mode(), [] { return "the controller"; });
  }
  auto const comm = communication();
  if(comm == simplex_rx)
  {
    return;
  }
  if(get(regs_, underrun_) != 0 or state_.tx_fifo.empty())
  {
    update(regs_, underrun_, 1);
    state_.host_shift = wire_frame(
        get(regs_, underrun_config_) == 0 ? get(regs_, underrun_data_)
        : comm == simplex_tx              ? 0u
                                          : state_.last_received);
    return;
  }
  state_.host_shift = wire_frame(state_.tx_fifo.front());
  state_.tx_fifo.erase(state_.tx_fifo.begin());
}

void spi::end_host_frame()
{
  auto const& now  = state_.host.front();
  state_.host_busy = 0;
  if(state_.host_taking == 0)
  {
    return;
  }
  if(now.bits != frame_bits() and not std::exchange(size_logged_, true))
  {
    warn(
        "SPI_CFG1",
        std::format(
            "the SPI controller sends {}-bit frames to a {}-bit DSIZE: each frame lands masked to "
            "DSIZE + 1 bits",
            now.bits, frame_bits()));
  }
  auto const in        = wire_frame(now.frames[state_.host_next]);
  state_.last_received = in;
  if(communication() != simplex_tx)
  {
    if(state_.rx_fifo.size() < capacity())
    {
      state_.rx_fifo.push_back(in);
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
    update(regs_, end_, 1);
  }
}

void spi::host_edge_reached()
{
  if(state_.host.empty())
  {
    return;
  }
  auto heard = std::optional<heard_frame>{};
  auto at_ps = state_.host.front().start_ps;
  if(state_.host_busy != 0)
  {
    auto const& now = state_.host.front();
    at_ps           = now.start_ps + (state_.host_next + 1) * now.frame_ps;
    heard           = heard_frame{
        .sent     = now.frames[state_.host_next],
        .received = state_.host_shift & mask_of(now.bits),
        .at_ps    = at_ps};
    end_host_frame();
    ++state_.host_next;
  }
  auto begins = true;
  if(state_.host_next == state_.host.front().frames.size())
  {
    state_.host.erase(state_.host.begin());
    state_.host_next     = 0;
    state_.host_selected = 0;
    state_.host_synced   = 0;
    begins               = not state_.host.empty() and state_.host.front().start_ps == at_ps;
    if(not state_.host.empty() and not begins)
    {
      ctx_.events.schedule_at(
          vtime{state_.host.front().start_ps}, host_edge, state_.host_generation, core::host_lane,
          ctx_.clock);
    }
  }
  if(begins)
  {
    auto const& now = state_.host.front();
    if(state_.host_next == 0)
    {
      state_.host_mode_checked = 0;
      if(get(regs_, enable_) != 0 and faults_on_select())
      {
        update(regs_, mode_fault_flag_, 1);
        update(regs_, enable_, 0);
        update(regs_, master_, 0);
        disable();
      }
      state_.host_selected = 1;
      state_.host_synced   = get(regs_, enable_) != 0 and get(regs_, master_) == 0 ? 1 : 0;
    }
    begin_host_frame();
    ctx_.events.schedule_at(
        vtime{now.start_ps + (state_.host_next + 1) * now.frame_ps}, host_edge,
        state_.host_generation, core::host_lane, ctx_.clock);
  }
  flags();
  if(heard and controller_)
  {
    controller_->frame(heard->sent, heard->received, vtime{heard->at_ps});
  }
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
  state_.mode_checked.resize(attachments_.size());
  subscribe();
}
}

auto spi_factory(board const& b) -> model_factory
{
  return [devices = b.spi, controllers = b.spi_controllers](
             model_context& ctx, peripheral_desc const& desc) -> std::shared_ptr<model> {
    auto const found      = devices.find(desc.name);
    auto const controller = controllers.find(desc.name);
    return std::make_shared<spi>(
        ctx, desc, found != devices.end() ? found->second : std::vector<spi_attachment>{},
        controller != controllers.end() ? controller->second : nullptr);
  };
}
}
