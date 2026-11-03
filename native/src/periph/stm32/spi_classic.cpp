#include "dma.hpp"
#include "dma_signal.hpp"
#include "factories.hpp"
#include "model_base.hpp"
#include "spi_bus.hpp"
#include "variants.hpp"

#include <popcorn/core/clock.hpp>
#include <popcorn/core/scheduler.hpp>
#include <popcorn/periph/spi.hpp>
#include <popcorn/periph/stm32/board.hpp>
#include <popcorn/periph/stm32/gpio.hpp>
#include <popcorn/periph/stm32/spi.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
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
constexpr std::uint32_t frame_done = 0;
constexpr std::uint32_t host_edge  = 1;

struct spi_fifo_state
{
  std::array<std::uint8_t, 4> rx{};
  std::array<std::uint8_t, 4> tx{};
  std::uint8_t                rx_level{};
  std::uint8_t                tx_level{};
  std::uint8_t                ldma_tx_last{};
  std::uint8_t                ldma_rx_last{};
  std::uint8_t                crc_left{};
  std::uint32_t               crc_in{};
  std::uint8_t                crc_restart{};

  friend auto operator==(spi_fifo_state const&, spi_fifo_state const&) -> bool = default;
};

struct spi_fifo_fields
{
  field_ref threshold;
  field_ref rx_level;
  field_ref tx_level;
  field_ref crc_length;
  field_ref last_tx;
  field_ref last_rx;
  field_ref pulse;
};

auto fifo_pop(std::array<std::uint8_t, 4>& queue, std::uint8_t& level, std::uint8_t count)
    -> std::uint32_t
{
  auto const taken = std::min(count, level);
  auto       value = std::uint32_t{};
  for(std::uint8_t index = 0; index < taken; ++index)
  {
    value |= std::uint32_t{queue[index]} << (8u * index);
  }
  std::shift_left(queue.begin(), queue.end(), taken);
  std::fill(queue.end() - taken, queue.end(), std::uint8_t{});
  level = static_cast<std::uint8_t>(level - taken);
  return value;
}

auto fifo_push(
    std::array<std::uint8_t, 4>& queue,
    std::uint8_t&                level,
    std::uint32_t                value,
    std::uint8_t                 count) -> bool
{
  if(std::size_t{level} + count > queue.size())
  {
    return false;
  }
  for(std::uint8_t index = 0; index < count; ++index)
  {
    queue[level++] = static_cast<std::uint8_t>(value >> (8u * index));
  }
  return true;
}

struct spi_classic_state
{
  std::uint32_t                       generation{};
  std::uint32_t                       tx{};
  std::uint8_t                        tx_full{};
  std::uint32_t                       rx{};
  std::uint8_t                        rx_full{};
  std::uint32_t                       shift{};
  std::uint64_t                       frame_end_ps{};
  std::uint8_t                        busy{};
  std::uint8_t                        crc_frame{};
  std::uint8_t                        crc_pending{};
  std::uint8_t                        crc_armed{};
  std::uint8_t                        overrun_read{};
  std::uint8_t                        fault_seen{};
  std::uint8_t                        selecting{};
  std::uint32_t                       last_received{};
  std::vector<std::uint8_t>           selected;
  std::vector<std::uint8_t>           mode_checked;
  std::vector<std::vector<std::byte>> devices;
  std::vector<spi_host_transfer>      host;
  std::uint32_t                       host_generation{};
  std::uint32_t                       host_next{};
  std::uint32_t                       host_shift{};
  std::uint8_t                        host_busy{};
  std::uint8_t                        host_taking{};
  std::uint8_t                        host_selected{};
  std::uint8_t                        host_mode_checked{};
  spi_fifo_state                      fifo{};

  friend auto operator==(spi_classic_state const&, spi_classic_state const&) -> bool = default;
};

auto kernel_name(peripheral_desc const& desc, spi_classic_variant const& variant) -> std::string
{
  auto const* const bus = window_of(variant.kernel_buses, static_cast<std::uint32_t>(desc.base));
  if(bus == nullptr)
  {
    throw config_error(
        desc.name, 0, "base", "an address on one of the buses that clock the SPI model");
  }
  return std::string{bus->clock};
}

/**
 * @brief SPI1 to SPI3 of the STM32L1, RM0038 section 28 "Serial peripheral interface (SPI)", and
 *        of the STM32L4, in SPI mode, controller and target. On the STM32L1, one transmit buffer,
 *        one receive buffer and one shift register, frames of 8 bits with CR1.DFF 0 and 16 bits
 *        with DFF 1, RM0038 28.3.1.
 *
 *        Controller mode, MSTR 1 and SPE 1: a frame starts when DR is written and the shifter is
 *        free, TXE rising as the word moves to the shifter, and lasts DFF bits of the APB clock
 *        of the instance's bus divided by 2 << BR, rounded down to a whole picosecond. At its end
 *        the frame is exchanged with the selected devices and lands in the receive buffer:
 *        RXNE, or OVR with the buffer kept while RXNE or OVR is still set, every later frame lost
 *        until the OVR clear, RM0038 28.3.10. A word written while a frame shifts starts the
 *        next frame at its end, with BSY kept 1. In receive-only mode, RXONLY 1, or
 *        bidirectional receive, BIDIMODE 1 and BIDIOE 0, frames run back to back from SPE 1, MOSI
 *        sending all ones, RM0038 28.3.5. Bidirectional transmit, BIDIMODE 1 and BIDIOE 1,
 *        receives nothing. BSY is 1 while a frame shifts or the transmit buffer holds a word with
 *        SPE 1, always 1 in controller receive-only mode and 0 in controller bidirectional
 *        receive, RM0038 28.3.7. Clearing SPE lets the frame in flight finish and land, then no
 *        frame starts, inferred. Both buffers keep their word across SPE 0 and a word written
 *        with SPE 0 waits for SPE 1, inferred. Frames cross the bus in wire order, the DR value
 *        bit reversed over the frame with LSBFIRST 1. A device without a chip select pin is
 *        selected by the SS output from the first frame to SPE 0 while CR2.SSOE is 1, RM0038
 *        28.3.1. A device or controller declaring an SPI mode is checked against CPOL << 1 |
 *        CPHA once per selection, as the STM32U5 SPI model does, and a mismatch fails the run
 *        with config_error.
 *
 *        Target mode, MSTR 0: the spi_controller_sink of the instance clocks it through
 *        spi_controller_port::transfer, each frame time fixed by the controller, its events on
 *        core::host_lane. The SPI takes part in a frame that starts with SPE 1 and NSS low: the
 *        controller's select with SSM 0, SSI with SSM 1. It drives the transmit buffer, or with
 *        the buffer empty the last frame it received, 0 before any, inferred, and all ones in
 *        receive-only mode. A frame of another size than DFF is shifted with the controller's
 *        size and masked, logged once. A controller-mode SPI selected by the controller with
 *        SSM 0 and SSOE 0, or set to MSTR 1 and SPE 1 with SSM 1 and SSI 0, takes a mode fault:
 *        MODF set, SPE and MSTR cleared, a frame in flight dropped, RM0038 28.3.10. An SR read
 *        or write with MODF set, then a CR1 write, clears MODF. While MODF is set, and in that
 *        clearing write, SPE and MSTR cannot be set, inferred for the clearing write. The
 *        controller attached to an instance is its only device.
 *
 *        CRC, RM0038 28.3.6: CRCEN written from 0 to 1 clears RXCRCR and TXCRCR. Each data frame
 *        sent updates TXCRCR, each data frame received RXCRCR, serially on each bit in wire
 *        order, LSB of the DR value first with LSBFIRST 1, from 0 with the low DFF bits of CRCPR
 *        as polynomial. RXCRCR takes the frames lost while OVR is set, inferred. With MSTR 0 it
 *        also takes the frames the controller clocks while NSS is high or SPE is 0, RM0038
 *        28.3.6, and those frames land nowhere else. At the end of a data frame with CRCNEXT 1,
 *        or after a completed transmit DMA transfer of the instance with CRCEN 1, and the
 *        transmit buffer empty or the SPI receive-only, the next frame is the CRC frame: it
 *        shifts TXCRCR out as a data word, bit reversed with LSBFIRST 1, inferred, and lands in
 *        the receive buffer, and CRCERR is set when the frame received differs from RXCRCR. A
 *        completed receive DMA transfer in receive-only mode makes the frame in flight the CRC
 *        frame, inferred, or the next frame with none in flight, controller and target alike.
 *        The CRC frame's end clears CRCNEXT, inferred: ST's HAL never clears it. CRCERR is
 *        cleared by a write of 0. TXCRCR and RXCRCR run on across a CRC phase, no data frame
 *        clears them, inferred, RM0038 28.3.6 clears them only on the CRCEN write.
 *
 *        OVR is cleared by a DR read followed by an SR read, RM0038 28.3.10. The SPI IRQ follows
 *        TXE with TXEIE, RXNE with RXNEIE and OVR, MODF or CRCERR with ERRIE, Table 159. With
 *        CR2.TXDMAEN the `<instance>_tx_dma` request follows TXE, with RXDMAEN the
 *        `<instance>_rx_dma` request follows RXNE, RM0038 28.3.9. Debugger accesses to DR and SR
 *        have no side effect: a debugger read pops no word and takes no part in the OVR or MODF
 *        clear, a debugger DR write sends nothing and a debugger SR write takes no part in the
 *        MODF clear. A write of 0 to CRCERR clears it from the CPU and the debugger alike.
 *        `SPI_I2SCFGR` and `SPI_I2SPR` are not claimed and stay storage. With the TI mode of the
 *        spi_classic_variant, FRF 1 with SPE 1 is logged once, raises model_error under
 *        strict_peripherals at each CR1 or CR2 write, and the SPI runs the Motorola format. The
 *        devices, the controller queue and both buffers are saved with the model, a reset drops
 *        the queue.
 *
 *        STM32L4 SPI1 to SPI3, RM0351 section 42, the fifo spi_classic_variant: the same rules
 *        with a 4-byte TXFIFO and RXFIFO in place of the buffers, RM0351 42.4.9. A frame has
 *        CR2.DS + 1 bits, one byte up to 8 bits, two bytes low byte first above, and a CR2
 *        write of DS 0000 to 0010 stores 0111, RM0351 42.6.2. A DR write pushes one byte at 8
 *        bits and two bytes at 16 or 32 bits, a DR read pops as many, the 32-bit access as the
 *        16-bit one, inferred. A DR write whose bytes do not all fit the TXFIFO is dropped whole
 *        and the stored bytes stay, logged once, model_error under strict_peripherals at the
 *        write, inferred: RM0351 42.4.9 clears TXE above half full because a 16-bit write over
 *        three stored 8-bit frames could cause "possible corruption of 3x 8-bit data frames
 *        already stored in the TXFIFO", and silicon may corrupt them where popcorn keeps them. A
 *        frame starts once the TXFIFO holds its bytes, so with DS above 8 a byte write waits for
 *        the next one, inferred. A DR read returns the oldest two bytes, 0 in an empty slot, and
 *        pops no more than the RXFIFO holds, inferred. Byte accesses to DR + 1 to DR + 3 move
 *        no FIFO byte, inferred. TXE is 1 while the TXFIFO holds 2 bytes or less, RXNE while
 *        the RXFIFO holds 1 byte with CR2.FRXTH 1 or 2 bytes with FRXTH 0, RM0351 42.4.10.
 *        FTLVL and FRLVL give the level up to 3, 11 at 3 bytes and at 4, inferred for FRLVL. A
 *        frame lands with OVR set or too little room is lost and sets OVR, RM0351 42.4.10. BSY
 *        is 1 while a frame shifts or, with SPE 1, the TXFIFO holds the bytes of a whole frame,
 *        RM0351 42.4.10, and 1 in controller receive-only mode and in controller bidirectional
 *        receive, RM0351 42.4.10 note. Both FIFOs keep their bytes across SPE 0, RM0351 42.4.9
 *        for the RXFIFO, inferred for the TXFIFO. An RCC reset of the instance empties them.
 *
 *        The STM32L4 CRC is 8 bits with CR1.CRCL 0 and 16 bits with CRCL 1, over the DS bits of
 *        each data frame, RM0351 42.4.14. The CRC phase is two 8-bit frames with DS 8 and CRCL
 *        1, TXCRCR[15:8] first with LSBFIRST 0 and [7:0] first with LSBFIRST 1, inferred, else
 *        one frame, with DS 16 and CRCL 0 a 16-bit frame holding the CRC-8, inferred. The CRC
 *        frames land in the RXFIFO and CRCERR compares them with RXCRCR after the last one. The
 *        first data frame after a CRC phase clears TXCRCR and RXCRCR before its own step,
 *        RM0351 42.4.14. In receive-only mode with CRCEN the RXFIFO holds one frame and FRLVL
 *        reads 00, RM0351 42.4.9 and 42.6.3. CRCEN with DS other than 8 or 16 logs once and
 *        raises model_error under strict_peripherals at the frame start, RM0351 42.4.14. With
 *        LDMA_TX, dma_requester::last_but_one() on the transmit request makes the next 16-bit
 *        DR write push its low byte only, with LDMA_RX on the receive request RXNE rises at one
 *        byte and the next 16-bit DR read pops one byte, for DS 8 or less, RM0351 42.4.9. That
 *        read returns the byte in [7:0] and 0 above, inferred. last_but_one() arms the flag only
 *        with DS 8 or less, the next DR access of its side takes it whatever its width, and SPE
 *        written to 0 clears it, inferred. With NSSP 1, MSTR 1, CPHA 0 and FRF 0 the SS output
 *        deselects after each frame and the next frame starts one SCK period after the previous
 *        end, exactly one, inferred, RM0351 42.4.12.
 *
 *        Devices and controller, the STM32L1 SPI on the generic interfaces: each frame is
 *        exchanged through spi_target::exchange_deferred() at its end, the frame's generation as
 *        ticket. The model offers no spi_target_port and calls neither spi_target::attached()
 *        nor spi_target::detached(). A device that leaves its reply pending drives all ones in
 *        that frame, which lands and runs on, and the frame's event then fails the run with
 *        config_error. A controller drives the instance through spi_controller_port::transfer()
 *        only: spi_controller_port::select(), frame(), deselect() and drop() throw config_error.
 */
class spi_classic final : public dispatch_model<spi_classic, spi_classic_state>,
                          public spi_controller_port,
                          public bus_reset_listener,
                          public dma_requester
{
public:
  spi_classic(
      model_context&                       ctx,
      peripheral_desc const&               desc,
      std::vector<spi_attachment>          attachments,
      std::shared_ptr<spi_controller_sink> controller,
      spi_classic_variant const&           variant);

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
  void select(vtime at, std::uint32_t lane, std::uint64_t unit) override;
  void frame(
      std::uint32_t word,
      std::uint8_t  bits,
      std::uint64_t frame_ps,
      vtime         at,
      std::uint32_t lane,
      std::uint64_t unit) override;
  void deselect(vtime at, std::uint32_t lane, std::uint64_t unit) override;
  void drop(vtime at, std::uint32_t lane, std::uint64_t unit) override;
  void transfer_completed(dma_controller const& by, std::uint32_t request) override;
  void last_but_one(dma_controller const& by, std::uint32_t request) override;

  void control_written(std::uint32_t old, std::uint32_t now);
  void status_written(std::uint32_t old, std::uint32_t now);
  void plain_written(std::uint32_t old, std::uint32_t now);
  void control2_written(std::uint32_t old, std::uint32_t now);

private:
  void               restore_reset_values(std::optional<reset_kind> kind);
  [[nodiscard]] auto frame_bits() const -> std::uint32_t;
  [[nodiscard]] auto frame_bytes() const -> std::uint8_t;
  [[nodiscard]] auto crc_bits() const -> std::uint32_t;
  [[nodiscard]] auto crc_frames() const -> std::uint8_t;
  [[nodiscard]] auto crc_bypass() const -> bool;
  [[nodiscard]] auto pulsing() const -> bool;
  void               begin_crc_frame();
  void               check_crc_size();
  [[nodiscard]] auto wire_frame(std::uint32_t value) const -> std::uint32_t;
  [[nodiscard]] auto receive_only() const -> bool;
  [[nodiscard]] auto receiving() const -> bool;
  [[nodiscard]] auto controlling() const -> bool;
  [[nodiscard]] auto frame_ps() -> std::uint64_t;
  [[nodiscard]] auto power_on_offset() const -> std::uint64_t;
  [[nodiscard]] auto bus_mode() const -> std::uint8_t;
  [[nodiscard]] auto crc_step(
      std::uint32_t crc,
      std::uint32_t frame,
      std::uint32_t frame_bits,
      std::uint32_t crc_bits) const -> std::uint32_t;
  [[nodiscard]] auto tx_ready() const -> bool;
  [[nodiscard]] auto tx_empty() const -> bool;
  [[nodiscard]] auto tx_holding() const -> bool;
  [[nodiscard]] auto rx_ready() const -> bool;
  [[nodiscard]] auto take_tx() -> std::uint32_t;
  void               queue_tx(std::uint32_t offset, std::uint8_t size, std::uint32_t value);
  void               store_rx(std::uint32_t received);
  [[nodiscard]] auto read_rx(std::uint32_t offset, std::uint8_t size) -> std::uint32_t;
  void               rethrow_refused();
  [[noreturn]] void  refuse_unit(std::string_view primitive) const;
  void               hardware_select(bool selected, std::optional<std::uint64_t> at_ps = {});
  void               start_frame(std::uint64_t from_ps);
  void               finish_frame();
  void               land(std::uint32_t received);
  void               mode_fault();
  void               warn_ti();
  void               flags();
  void               host_edge_reached();
  void               begin_host_frame();
  void               end_host_frame();
  [[nodiscard]] auto host_selects() const -> bool;

  register_file&                       regs_;
  std::shared_ptr<spi_controller_sink> controller_;
  spi_attachments<spi_classic_state>   attachments_;
  reg_ref                              control_;
  reg_ref                              status_;
  reg_ref                              data_;
  reg_ref                              tx_crc_;
  reg_ref                              rx_crc_;
  field_ref                            phase_;
  field_ref                            polarity_;
  field_ref                            master_;
  field_ref                            baud_;
  field_ref                            enable_;
  field_ref                            lsb_first_;
  field_ref                            select_input_;
  field_ref                            software_select_;
  field_ref                            rx_only_;
  field_ref                            size_;
  field_ref                            crc_next_;
  field_ref                            crc_enable_;
  field_ref                            bidi_output_;
  field_ref                            bidi_;
  field_ref                            rx_dma_enable_;
  field_ref                            tx_dma_enable_;
  field_ref                            ss_output_;
  field_ref                            error_ie_;
  field_ref                            rx_ie_;
  field_ref                            tx_ie_;
  std::optional<field_ref>             ti_format_;
  std::optional<spi_fifo_fields>       fifo_;
  field_ref                            rx_flag_;
  field_ref                            tx_flag_;
  field_ref                            crc_error_;
  field_ref                            fault_;
  field_ref                            overrun_;
  field_ref                            busy_;
  field_ref                            polynomial_;
  core::clock_domain                   kernel_;
  irq_line&                            line_;
  dma_signal                           tx_dma_;
  dma_signal                           rx_dma_;
  std::vector<std::string>             mismatched_;
  std::vector<std::string>             deferred_;
  bool                                 ti_logged_{};
  bool                                 no_clock_logged_{};
  bool                                 size_logged_{};
  bool                                 dropped_logged_{};
  bool                                 crc_size_logged_{};
};

constexpr std::array<write_entry<spi_classic>, 7> spi_classic_writes{
    {
     {"CR1", &spi_classic::control_written},
     {"CR2", &spi_classic::control2_written},
     {"SR", &spi_classic::status_written},
     {"DR", &spi_classic::plain_written},
     {"CRCPR", nullptr},
     {"RXCRCR", nullptr},
     {"TXCRCR", nullptr},
     }
};

spi_classic::spi_classic(
    model_context&                       ctx,
    peripheral_desc const&               desc,
    std::vector<spi_attachment>          attachments,
    std::shared_ptr<spi_controller_sink> controller,
    spi_classic_variant const&           variant)
    : dispatch_model{ctx, desc, "SPI", spi_classic_writes}, regs_{ctx.regs},
      controller_{std::move(controller)}, attachments_{state_, std::move(attachments)},
      control_{reg("CR1")}, status_{reg("SR")}, data_{reg("DR")}, tx_crc_{reg("TXCRCR")},
      rx_crc_{reg("RXCRCR")}, phase_{field("CR1", "CPHA")}, polarity_{field("CR1", "CPOL")},
      master_{field("CR1", "MSTR")}, baud_{field("CR1", "BR")}, enable_{field("CR1", "SPE")},
      lsb_first_{field("CR1", "LSBFIRST")}, select_input_{field("CR1", "SSI")},
      software_select_{field("CR1", "SSM")}, rx_only_{field("CR1", "RXONLY")},
      size_{variant.fifo ? field("CR2", "DS") : field("CR1", "DFF")},
      crc_next_{field("CR1", "CRCNEXT")}, crc_enable_{field("CR1", "CRCEN")},
      bidi_output_{field("CR1", "BIDIOE")}, bidi_{field("CR1", "BIDIMODE")},
      rx_dma_enable_{field("CR2", "RXDMAEN")}, tx_dma_enable_{field("CR2", "TXDMAEN")},
      ss_output_{field("CR2", "SSOE")}, error_ie_{field("CR2", "ERRIE")},
      rx_ie_{field("CR2", "RXNEIE")}, tx_ie_{field("CR2", "TXEIE")},
      ti_format_{variant.ti_mode ? std::optional{field("CR2", "FRF")} : std::nullopt},
      fifo_{
          variant.fifo ? std::optional{spi_fifo_fields{
                             .threshold  = field("CR2", "FRXTH"),
                             .rx_level   = field("SR", "FRLVL"),
                             .tx_level   = field("SR", "FTLVL"),
                             .crc_length = field("CR1", "CRCL"),
                             .last_tx    = field("CR2", "LDMA_TX"),
                             .last_rx    = field("CR2", "LDMA_RX"),
                             .pulse      = field("CR2", "NSSP")}}
                       : std::nullopt},
      rx_flag_{field("SR", "RXNE")}, tx_flag_{field("SR", "TXE")},
      crc_error_{field("SR", "CRCERR")}, fault_{field("SR", "MODF")}, overrun_{field("SR", "OVR")},
      busy_{field("SR", "BSY")}, polynomial_{field("CRCPR", "CRCPOLY")},
      kernel_{ctx.clock.domain(kernel_name(desc, variant))}, line_{irq(desc.name + "_IRQn")},
      tx_dma_{ctx.peers, lower(desc.name) + "_tx_dma"},
      rx_dma_{ctx.peers, lower(desc.name) + "_rx_dma"}
{
  if(not fifo_)
  {
    expect_field("CR2", "FRXTH", false);
  }
  if(controller_ and not attachments_.empty())
  {
    throw config_error(
        ctx.part.part, 0, std::format("board.spi.{}", desc.name),
        "an SPI controller alone on the instance, no device next to it");
  }
}

auto spi_classic::frame_bits() const -> std::uint32_t
{
  if(fifo_)
  {
    return get(regs_, size_) + 1;
  }
  return get(regs_, size_) != 0 ? 16 : 8;
}

auto spi_classic::frame_bytes() const -> std::uint8_t
{
  return frame_bits() > 8 ? 2 : 1;
}

auto spi_classic::crc_bits() const -> std::uint32_t
{
  if(fifo_)
  {
    return get(regs_, fifo_->crc_length) != 0 ? 16 : 8;
  }
  return frame_bits();
}

auto spi_classic::crc_frames() const -> std::uint8_t
{
  return crc_bits() > frame_bits() ? 2 : 1;
}

auto spi_classic::crc_bypass() const -> bool
{
  return fifo_ and receive_only() and get(regs_, crc_enable_) != 0;
}

auto spi_classic::pulsing() const -> bool
{
  return fifo_
     and get(regs_, fifo_->pulse) != 0
     and get(regs_, phase_) == 0
     and (not ti_format_ or get(regs_, *ti_format_) == 0);
}

void spi_classic::begin_crc_frame()
{
  auto& fifo         = state_.fifo;
  state_.crc_pending = 0;
  state_.crc_frame   = 1;
  if(fifo.crc_left == 0)
  {
    fifo.crc_left = crc_frames();
    fifo.crc_in   = 0;
  }
  auto const index = static_cast<std::uint32_t>(crc_frames() - fifo.crc_left);
  auto const lane  = get(regs_, lsb_first_) != 0 ? index : crc_frames() - 1u - index;
  auto const width = crc_frames() == 1 ? crc_bits() : 8u;
  state_.shift     = regs_.peek(tx_crc_.offset) >> (8u * lane) & spi_mask(width);
}

void spi_classic::check_crc_size()
{
  if(not fifo_ or get(regs_, crc_enable_) == 0 or frame_bits() == 8 or frame_bits() == 16)
  {
    return;
  }
  if(not std::exchange(crc_size_logged_, true))
  {
    warn(
        "CR2", "CRCEN 1 with a CR2.DS other than 8 or 16 bits: not modelled, the CRC runs over the "
               "DS bits");
  }
  if(ctx_.strict_peripherals)
  {
    throw model_error(std::string{desc_.name}, "CR2", reg("CR2").offset);
  }
}

auto spi_classic::wire_frame(std::uint32_t value) const -> std::uint32_t
{
  auto const frame = value & spi_mask(frame_bits());
  return get(regs_, lsb_first_) != 0 ? spi_reversed(frame, frame_bits()) : frame;
}

auto spi_classic::receive_only() const -> bool
{
  return get(regs_, bidi_) != 0 ? get(regs_, bidi_output_) == 0 : get(regs_, rx_only_) != 0;
}

auto spi_classic::receiving() const -> bool
{
  return get(regs_, bidi_) == 0 or get(regs_, bidi_output_) == 0;
}

auto spi_classic::controlling() const -> bool
{
  return get(regs_, enable_) != 0 and get(regs_, master_) != 0;
}

auto spi_classic::power_on_offset() const -> std::uint64_t
{
  return ctx_.clock.since_power_on().ps - ctx_.clock.time().ps;
}

auto spi_classic::bus_mode() const -> std::uint8_t
{
  return static_cast<std::uint8_t>(get(regs_, polarity_) << 1 | get(regs_, phase_));
}

auto spi_classic::crc_step(
    std::uint32_t crc,
    std::uint32_t frame,
    std::uint32_t frame_bits,
    std::uint32_t crc_bits) const -> std::uint32_t
{
  auto const top  = std::uint32_t{1} << (crc_bits - 1);
  auto const poly = get(regs_, polynomial_) & spi_mask(crc_bits);
  for(std::uint32_t bit = frame_bits; bit-- > 0;)
  {
    auto const feedback = ((crc & top) != 0) != ((frame >> bit & 1u) != 0);
    crc                 = (crc << 1) & spi_mask(crc_bits);
    if(feedback)
    {
      crc ^= poly;
    }
  }
  return crc;
}

auto spi_classic::tx_ready() const -> bool
{
  if(fifo_)
  {
    return state_.fifo.tx_level >= frame_bytes();
  }
  return state_.tx_full != 0;
}

auto spi_classic::tx_empty() const -> bool
{
  if(fifo_)
  {
    return state_.fifo.tx_level <= 2;
  }
  return state_.tx_full == 0;
}

auto spi_classic::tx_holding() const -> bool
{
  if(fifo_)
  {
    return state_.fifo.tx_level != 0;
  }
  return state_.tx_full != 0;
}

auto spi_classic::rx_ready() const -> bool
{
  if(fifo_)
  {
    auto const& fifo = state_.fifo;
    if(crc_bypass())
    {
      return fifo.rx_level >= frame_bytes();
    }
    auto const last =
        fifo.ldma_rx_last != 0 and get(regs_, rx_dma_enable_) != 0 and frame_bytes() == 1;
    return fifo.rx_level >= (get(regs_, fifo_->threshold) != 0 or last ? 1 : 2);
  }
  return state_.rx_full != 0;
}

auto spi_classic::take_tx() -> std::uint32_t
{
  if(fifo_)
  {
    return fifo_pop(state_.fifo.tx, state_.fifo.tx_level, frame_bytes()) & spi_mask(frame_bits());
  }
  state_.tx_full = 0;
  return state_.tx;
}

void spi_classic::queue_tx(std::uint32_t offset, std::uint8_t size, std::uint32_t value)
{
  if(fifo_)
  {
    if(offset != data_.offset)
    {
      return;
    }
    auto       count = static_cast<std::uint8_t>(size == 1 ? 1 : 2);
    auto const last  = std::exchange(state_.fifo.ldma_tx_last, 0) != 0;
    if(count == 2 and frame_bytes() == 1 and last)
    {
      count = 1;
    }
    if(fifo_push(state_.fifo.tx, state_.fifo.tx_level, value, count))
    {
      return;
    }
    if(not std::exchange(dropped_logged_, true))
    {
      warn("DR", "a DR write that does not fit the 4-byte TXFIFO: the write is dropped whole");
    }
    if(ctx_.strict_peripherals)
    {
      throw model_error(std::string{desc_.name}, "DR", data_.offset);
    }
    return;
  }
  auto const merged = merge_write(regs_.peek(data_.offset), offset, size, value);
  state_.tx         = merged & spi_mask(frame_bits());
  state_.tx_full    = 1;
}

void spi_classic::store_rx(std::uint32_t received)
{
  if(fifo_)
  {
    auto const capacity = crc_bypass() ? frame_bytes() : state_.fifo.rx.size();
    auto const level    = std::size_t{state_.fifo.rx_level};
    if(get(regs_, overrun_) != 0 or level + frame_bytes() > capacity)
    {
      update(regs_, overrun_, 1);
      return;
    }
    fifo_push(state_.fifo.rx, state_.fifo.rx_level, received, frame_bytes());
    return;
  }
  if(state_.rx_full != 0 or get(regs_, overrun_) != 0)
  {
    update(regs_, overrun_, 1);
    return;
  }
  state_.rx      = received;
  state_.rx_full = 1;
}

auto spi_classic::read_rx(std::uint32_t offset, std::uint8_t size) -> std::uint32_t
{
  if(not fifo_)
  {
    state_.rx_full = 0;
    return ~std::uint32_t{};
  }
  if(offset != data_.offset)
  {
    return ~std::uint32_t{};
  }
  auto       count = static_cast<std::uint8_t>(size == 1 ? 1 : 2);
  auto const last  = std::exchange(state_.fifo.ldma_rx_last, 0) != 0;
  if(count == 2 and frame_bytes() == 1 and last)
  {
    count = 1;
  }
  fifo_pop(state_.fifo.rx, state_.fifo.rx_level, count);
  return spi_mask(8u * count);
}

void spi_classic::rethrow_refused()
{
  auto const joined = [](std::vector<std::string> const& names) {
    auto out = std::string{};
    for(auto const& one : names)
    {
      out += out.empty() ? one : " and " + one;
    }
    return out;
  };
  if(not mismatched_.empty())
  {
    deferred_.clear();
    auto const declared = joined(std::exchange(mismatched_, {}));
    throw config_error(
        ctx_.part.part, 0, std::format("devices.{}", desc_.name),
        std::format(
            "{}, not mode {} the firmware set, CPOL {} CPHA {}", declared, bus_mode(),
            get(regs_, polarity_), get(regs_, phase_)));
  }
  if(not deferred_.empty())
  {
    auto const devices = joined(std::exchange(deferred_, {}));
    throw config_error(
        ctx_.part.part, 0, std::format("devices.{}", desc_.name),
        std::format(
            "a reply at the frame's end from {}, the STM32L1 SPI completes no deferred frame",
            devices));
  }
}

void spi_classic::refuse_unit(std::string_view primitive) const
{
  throw config_error(
      ctx_.part.part, 0, std::format("devices.{}", desc_.name),
      std::format("transfer() only, the STM32L1 SPI takes no {}()", primitive));
}

auto spi_classic::frame_ps() -> std::uint64_t
{
  auto const hz = ctx_.clock.hz(kernel_);
  if(hz == 0)
  {
    if(not std::exchange(no_clock_logged_, true))
    {
      warn("CR1", "the SPI bus clock is stopped: no frame is transferred");
    }
    return 0;
  }
  return std::uint64_t{frame_bits()} * (2u << get(regs_, baud_)) * ps_per_second / hz;
}

void spi_classic::hardware_select(bool selected, std::optional<std::uint64_t> at_ps)
{
  if(selected and get(regs_, ss_output_) == 0)
  {
    return;
  }
  state_.selecting = selected ? 1 : 0;
  auto const at    = at_ps ? vtime{*at_ps + power_on_offset()} : ctx_.clock.since_power_on();
  for(std::size_t index = 0; index < attachments_.size(); ++index)
  {
    if(attachments_[index].cs_pin or state_.selected[index] == (selected ? 1 : 0))
    {
      continue;
    }
    state_.selected[index]     = selected ? 1 : 0;
    state_.mode_checked[index] = 0;
    attachments_[index].device->select(selected, at);
  }
}

void spi_classic::reset(register_file&, reset_kind kind)
{
  restore_reset_values(kind);
}

void spi_classic::on_bus_reset()
{
  restore_reset_values(std::nullopt);
}

void spi_classic::restore_reset_values(std::optional<reset_kind> kind)
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
  hardware_select(false);
  attachments_.subscribe(ctx_, desc_.name);
  tx_dma_.resolve();
  rx_dma_.resolve();
  flags();
}

void spi_classic::power_cut()
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

void spi_classic::start_frame(std::uint64_t from_ps)
{
  if(state_.busy != 0 or not controlling())
  {
    return;
  }
  auto const crc = state_.crc_pending != 0;
  if(not crc and not receive_only() and not tx_ready())
  {
    return;
  }
  auto const ps = frame_ps();
  if(ps == 0)
  {
    return;
  }
  check_crc_size();
  if(pulsing() and state_.frame_end_ps != 0)
  {
    from_ps = std::max(
        from_ps,
        state_.frame_end_ps + (2u << get(regs_, baud_)) * ps_per_second / ctx_.clock.hz(kernel_));
  }
  if(crc)
  {
    begin_crc_frame();
  }
  else if(receive_only())
  {
    state_.shift = spi_mask(frame_bits());
  }
  else
  {
    state_.shift = take_tx();
  }
  if(state_.selecting == 0)
  {
    hardware_select(true, pulsing() ? std::optional{from_ps} : std::nullopt);
  }
  state_.busy         = 1;
  state_.frame_end_ps = std::max(from_ps + ps, ctx_.clock.time().ps);
  ctx_.events.schedule_at(vtime{state_.frame_end_ps}, frame_done, ++state_.generation, ctx_.clock);
}

void spi_classic::finish_frame()
{
  state_.busy    = 0;
  auto const at  = vtime{state_.frame_end_ps + power_on_offset()};
  auto const out = receive_only() ? spi_mask(frame_bits()) : wire_frame(state_.shift);
  auto       in  = spi_mask(frame_bits());
  for(std::size_t index = 0; index < attachments_.size(); ++index)
  {
    if(state_.selected[index] != 0)
    {
      attachments_.check_device(index, bus_mode(), mismatched_);
      auto const reply = attachments_[index].device->exchange_deferred(
          out, static_cast<std::uint8_t>(frame_bits()), at, state_.generation);
      if(reply)
      {
        in &= *reply;
      }
      else
      {
        deferred_.push_back(std::format("the device on {}", spi_pin_name(attachments_[index])));
      }
    }
  }
  land(wire_frame(in));
  if(get(regs_, enable_) == 0 or pulsing())
  {
    hardware_select(false);
  }
  if(get(regs_, enable_) != 0)
  {
    start_frame(state_.frame_end_ps);
  }
}

void spi_classic::land(std::uint32_t received)
{
  auto const bits  = frame_bits();
  auto const width = crc_bits();
  auto&      fifo  = state_.fifo;
  if(state_.crc_frame != 0)
  {
    state_.crc_frame = 0;
    fifo.crc_left    = std::max<std::uint8_t>(fifo.crc_left, 1);
    auto const index = static_cast<std::uint32_t>(crc_frames() - fifo.crc_left);
    auto const lane  = get(regs_, lsb_first_) != 0 ? index : crc_frames() - 1u - index;
    fifo.crc_in |= (received & spi_mask(crc_frames() == 1 ? width : 8u)) << (8u * lane);
    if(--fifo.crc_left != 0)
    {
      state_.crc_pending = 1;
    }
    else
    {
      state_.crc_armed = 0;
      update(regs_, crc_next_, 0);
      if(receiving()
         and (fifo.crc_in & spi_mask(width)) != (regs_.peek(rx_crc_.offset) & spi_mask(width)))
      {
        update(regs_, crc_error_, 1);
      }
      fifo.crc_restart = fifo_ ? 1 : 0;
    }
  }
  else if(get(regs_, crc_enable_) != 0)
  {
    if(std::exchange(fifo.crc_restart, 0) != 0)
    {
      regs_.poke(tx_crc_.offset, 0);
      regs_.poke(rx_crc_.offset, 0);
    }
    if(not receive_only())
    {
      regs_.poke(
          tx_crc_.offset,
          crc_step(regs_.peek(tx_crc_.offset), wire_frame(state_.shift), bits, width));
    }
    if(receiving())
    {
      regs_.poke(
          rx_crc_.offset, crc_step(regs_.peek(rx_crc_.offset), wire_frame(received), bits, width));
    }
    auto const armed = get(regs_, crc_next_) != 0 or state_.crc_armed != 0;
    if(armed and (receive_only() or not tx_holding()))
    {
      state_.crc_pending = 1;
    }
  }
  state_.last_received = received;
  if(not receiving())
  {
    return;
  }
  store_rx(received);
}

void spi_classic::mode_fault()
{
  update(regs_, fault_, 1);
  update(regs_, enable_, 0);
  update(regs_, master_, 0);
  if(state_.busy != 0)
  {
    ++state_.generation;
    state_.busy          = 0;
    state_.crc_frame     = 0;
    state_.fifo.crc_left = 0;
    state_.fifo.crc_in   = 0;
  }
  hardware_select(false);
}

void spi_classic::flags()
{
  auto const up         = get(regs_, enable_) != 0;
  auto const controller = up and get(regs_, master_) != 0;
  auto const shifting   = state_.busy != 0 or (state_.host_busy != 0 and state_.host_taking != 0);
  auto       busy       = shifting or (up and tx_ready());
  if(controller and receive_only())
  {
    busy = fifo_ or get(regs_, bidi_) == 0;
  }
  update(regs_, tx_flag_, tx_empty() ? 1 : 0);
  update(regs_, rx_flag_, rx_ready() ? 1 : 0);
  update(regs_, busy_, busy ? 1 : 0);
  if(fifo_)
  {
    auto const& fifo = state_.fifo;
    update(regs_, fifo_->rx_level, crc_bypass() ? 0 : std::min<std::uint32_t>(fifo.rx_level, 3));
    update(regs_, fifo_->tx_level, std::min<std::uint32_t>(fifo.tx_level, 3));
    regs_.poke(data_.offset, std::uint32_t{fifo.rx[0]} | std::uint32_t{fifo.rx[1]} << 8);
  }
  else
  {
    regs_.poke(data_.offset, state_.rx & spi_mask(frame_bits()));
  }
  auto const errors =
      get(regs_, overrun_) != 0 or get(regs_, fault_) != 0 or get(regs_, crc_error_) != 0;
  auto const level = (get(regs_, tx_ie_) != 0 and tx_empty())
                  or (get(regs_, rx_ie_) != 0 and rx_ready())
                  or (get(regs_, error_ie_) != 0 and errors);
  if(level)
  {
    line_.raise();
  }
  else
  {
    line_.lower();
  }
  tx_dma_.set(get(regs_, tx_dma_enable_) != 0 and tx_empty());
  rx_dma_.set(get(regs_, rx_dma_enable_) != 0 and rx_ready());
}

void spi_classic::control_written(std::uint32_t old, std::uint32_t now)
{
  auto const locked = get(regs_, fault_) != 0;
  if(locked and state_.fault_seen != 0)
  {
    state_.fault_seen = 0;
    update(regs_, fault_, 0);
  }
  if(locked)
  {
    update(regs_, enable_, enable_.of(old) & enable_.of(now));
    update(regs_, master_, master_.of(old) & master_.of(now));
    now = regs_.peek(control_.offset);
  }
  if(crc_enable_.of(old) == 0 and crc_enable_.of(now) != 0)
  {
    regs_.poke(tx_crc_.offset, 0);
    regs_.poke(rx_crc_.offset, 0);
    state_.crc_pending      = 0;
    state_.crc_armed        = 0;
    state_.fifo.crc_left    = 0;
    state_.fifo.crc_in      = 0;
    state_.fifo.crc_restart = 0;
  }
  if(enable_.of(old) != 0 and enable_.of(now) == 0)
  {
    state_.fifo.ldma_tx_last = 0;
    state_.fifo.ldma_rx_last = 0;
  }
  warn_ti();
  if(enable_.of(now) != 0 and master_.of(now) != 0)
  {
    auto const software = software_select_.of(now) != 0;
    if((software and select_input_.of(now) == 0)
       or (not software and get(regs_, ss_output_) == 0 and state_.host_selected != 0))
    {
      mode_fault();
      return;
    }
  }
  if(enable_.of(now) == 0)
  {
    if(state_.busy == 0)
    {
      hardware_select(false);
    }
    return;
  }
  start_frame(ctx_.clock.time().ps);
}

void spi_classic::status_written(std::uint32_t old, std::uint32_t now)
{
  auto const kept = crc_error_.of(old) & crc_error_.of(now);
  regs_.poke(status_.offset, crc_error_.with(old, kept));
}

void spi_classic::plain_written(std::uint32_t, std::uint32_t)
{}

void spi_classic::control2_written(std::uint32_t, std::uint32_t)
{
  if(fifo_ and get(regs_, size_) < 3)
  {
    update(regs_, size_, 7);
  }
  warn_ti();
}

void spi_classic::warn_ti()
{
  if(not ti_format_ or get(regs_, enable_) == 0 or get(regs_, *ti_format_) == 0)
  {
    return;
  }
  if(not std::exchange(ti_logged_, true))
  {
    warn("CR2", "the TI frame format, CR2.FRF 1: not modelled, the SPI runs the Motorola format");
  }
  if(ctx_.strict_peripherals)
  {
    throw model_error(std::string{desc_.name}, "CR2", reg("CR2").offset);
  }
}

auto spi_classic::on_read(
    register_file& regs,
    std::uint32_t  offset,
    std::uint8_t   size,
    access_ctx     access) -> std::expected<std::uint32_t, bus_error>
{
  auto const word = offset & ~3u;
  if(access.by == initiator::debugger or (word != data_.offset and word != status_.offset))
  {
    return dispatch_model::on_read(regs, offset, size, access);
  }
  auto const value = dispatch_model::on_read(regs, offset, size, access);
  auto       lanes = ~std::uint32_t{};
  if(word == status_.offset)
  {
    if(get(regs_, fault_) != 0)
    {
      state_.fault_seen = 1;
    }
    if(std::exchange(state_.overrun_read, 0) != 0)
    {
      update(regs_, overrun_, 0);
    }
  }
  else
  {
    state_.overrun_read = get(regs_, overrun_) != 0 ? 1 : 0;
    lanes               = read_rx(offset, size);
  }
  flags();
  return value.transform([lanes](std::uint32_t word_read) { return word_read & lanes; });
}

auto spi_classic::on_write(
    register_file& regs,
    std::uint32_t  offset,
    std::uint8_t   size,
    std::uint32_t  old,
    std::uint32_t  value,
    access_ctx     access) -> std::expected<void, bus_error>
{
  if((offset & ~3u) == data_.offset)
  {
    if(access.by == initiator::debugger)
    {
      return {};
    }
    queue_tx(offset, size, value);
    start_frame(ctx_.clock.time().ps);
    flags();
    return {};
  }
  if((offset & ~3u) == status_.offset
     and access.by != initiator::debugger
     and get(regs_, fault_) != 0)
  {
    state_.fault_seen = 1;
  }
  auto const result = dispatch_model::on_write(regs, offset, size, old, value, access);
  flags();
  return result;
}

void spi_classic::on_event(std::uint32_t kind, std::uint64_t payload)
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
  try
  {
    finish_frame();
  }
  catch(model_error const&)
  {
    flags();
    throw;
  }
  flags();
  rethrow_refused();
}

void spi_classic::transfer_completed(dma_controller const& by, std::uint32_t request)
{
  if(get(regs_, crc_enable_) == 0)
  {
    return;
  }
  if(tx_dma_.carries(by, request))
  {
    state_.crc_armed = 1;
  }
  else if(rx_dma_.carries(by, request) and receive_only())
  {
    auto const in_flight = state_.busy != 0 or (state_.host_busy != 0 and state_.host_taking != 0);
    if(in_flight and state_.crc_frame == 0)
    {
      state_.crc_frame     = 1;
      state_.fifo.crc_left = crc_frames();
      state_.fifo.crc_in   = 0;
    }
    else if(not in_flight)
    {
      state_.crc_pending = 1;
    }
  }
}

void spi_classic::last_but_one(dma_controller const& by, std::uint32_t request)
{
  if(not fifo_)
  {
    return;
  }
  if(frame_bytes() != 1)
  {
    return;
  }
  if(tx_dma_.carries(by, request) and get(regs_, fifo_->last_tx) != 0)
  {
    state_.fifo.ldma_tx_last = 1;
  }
  else if(rx_dma_.carries(by, request) and get(regs_, fifo_->last_rx) != 0)
  {
    state_.fifo.ldma_rx_last = 1;
  }
  flags();
}

auto spi_classic::transfer(
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
  auto const idle = state_.host.empty();
  auto const from = queue_host_transfer(
      state_.host, ctx_.part.part, desc_.name, ctx_.clock.time().ps, start, frame_ps, bits, frames);
  if(idle)
  {
    ctx_.events.schedule_at(from, host_edge, state_.host_generation, core::host_lane, ctx_.clock);
  }
  return from;
}

void spi_classic::select(vtime, std::uint32_t, std::uint64_t)
{
  refuse_unit("select");
}

void spi_classic::frame(
    std::uint32_t,
    std::uint8_t,
    std::uint64_t,
    vtime,
    std::uint32_t,
    std::uint64_t)
{
  refuse_unit("frame");
}

void spi_classic::deselect(vtime, std::uint32_t, std::uint64_t)
{
  refuse_unit("deselect");
}

void spi_classic::drop(vtime, std::uint32_t, std::uint64_t)
{
  refuse_unit("drop");
}

auto spi_classic::host_selects() const -> bool
{
  if(get(regs_, enable_) == 0 or get(regs_, master_) != 0)
  {
    return false;
  }
  if(get(regs_, software_select_) != 0)
  {
    return get(regs_, select_input_) == 0;
  }
  return state_.host_selected != 0;
}

void spi_classic::begin_host_frame()
{
  if(host_selects())
  {
    check_crc_size();
  }
  state_.host_busy   = 1;
  state_.host_taking = 0;
  state_.host_shift  = ~std::uint32_t{};
  if(not host_selects())
  {
    return;
  }
  state_.host_taking = 1;
  if(std::exchange(state_.host_mode_checked, 1) == 0)
  {
    spi_check_mode(mismatched_, bus_mode(), controller_->mode(), [] { return "the controller"; });
  }
  if(state_.crc_pending != 0)
  {
    begin_crc_frame();
  }
  if(receive_only())
  {
    return;
  }
  if(state_.crc_frame == 0 and tx_ready())
  {
    state_.shift = take_tx();
  }
  else if(state_.crc_frame == 0)
  {
    state_.shift = state_.last_received;
  }
  state_.host_shift = wire_frame(state_.shift);
}

void spi_classic::end_host_frame()
{
  auto const& now  = state_.host.front();
  state_.host_busy = 0;
  if(state_.host_taking == 0)
  {
    if(get(regs_, crc_enable_) != 0 and get(regs_, master_) == 0 and receiving())
    {
      regs_.poke(
          rx_crc_.offset,
          crc_step(
              regs_.peek(rx_crc_.offset), now.frames[state_.host_next], frame_bits(), crc_bits()));
    }
    return;
  }
  if(now.bits != frame_bits() and not std::exchange(size_logged_, true))
  {
    warn(
        "CR1",
        std::format(
            "the SPI controller sends {}-bit frames to a {}-bit {}: each frame lands masked to "
            "{} bits",
            now.bits, frame_bits(), fifo_ ? "DS" : "DFF", fifo_ ? "DS" : "DFF"));
  }
  land(wire_frame(now.frames[state_.host_next]));
}

void spi_classic::host_edge_reached()
{
  if(state_.host.empty())
  {
    return;
  }
  auto heard = std::optional<spi_heard_frame>{};
  auto at_ps = state_.host.front().start_ps;
  if(state_.host_busy != 0)
  {
    auto const& now = state_.host.front();
    at_ps           = now.start_ps + (state_.host_next + 1) * now.frame_ps;
    heard           = spi_heard_frame{
        .sent     = now.frames[state_.host_next],
        .received = state_.host_shift & spi_mask(now.bits),
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
      state_.host_selected     = 1;
      if(controlling() and get(regs_, software_select_) == 0 and get(regs_, ss_output_) == 0)
      {
        mode_fault();
      }
    }
    try
    {
      begin_host_frame();
    }
    catch(model_error const&)
    {
      flags();
      if(heard and controller_)
      {
        controller_->frame(heard->sent, heard->received, vtime{heard->at_ps});
      }
      throw;
    }
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

void spi_classic::save(std::vector<std::byte>& out) const
{
  auto copy = state_;
  attachments_.save_devices(copy.devices);
  reflex::serde::bson::serializer{out}.dump(copy);
}

void spi_classic::load(std::span<std::byte const> in)
{
  auto        loaded = reflex::serde::bson::deserializer{in}.template load<spi_classic_state>();
  auto const& fifo   = loaded.fifo;
  if(fifo.rx_level > fifo.rx.size() or fifo.tx_level > fifo.tx.size())
  {
    throw config_error(
        ctx_.part.part, 0, std::format("snapshot.{}.fifo", desc_.name),
        "a TXFIFO and an RXFIFO level of 4 bytes or less");
  }
  state_ = std::move(loaded);
  attachments_.load_devices(ctx_, desc_.name);
}
}

auto spi_classic_factory(board const& b, spi_classic_variant variant) -> model_factory
{
  return [devices = b.spi, controllers = b.spi_controllers,
          variant](model_context& ctx, peripheral_desc const& desc) -> std::shared_ptr<model> {
    auto const found      = devices.find(desc.name);
    auto const controller = controllers.find(desc.name);
    return std::make_shared<spi_classic>(
        ctx, desc, found != devices.end() ? found->second : std::vector<spi_attachment>{},
        controller != controllers.end() ? controller->second : nullptr, variant);
  };
}
}
