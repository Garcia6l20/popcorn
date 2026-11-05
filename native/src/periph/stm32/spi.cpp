#include "dma_signal.hpp"
#include "model_base.hpp"
#include "spi_bus.hpp"
#include "spi_link_port.hpp"
#include "spi_rows.hpp"
#include "target_port.hpp"

#include <popcorn/core/clock.hpp>
#include <popcorn/core/scheduler.hpp>
#include <popcorn/periph/reset_listener.hpp>
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
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
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
constexpr std::uint32_t frame_done  = 0;
constexpr std::uint32_t host_edge   = 1;
constexpr std::uint32_t completed   = 2;
constexpr std::uint32_t port_due    = 3;
constexpr std::uint32_t full_duplex = 0;
constexpr std::uint32_t simplex_tx  = 1;
constexpr std::uint32_t simplex_rx  = 2;

struct spi_completion
{
  std::uint64_t ticket{};
  std::uint32_t word{};
  std::uint64_t at_ps{};

  friend auto operator==(spi_completion const&, spi_completion const&) -> bool = default;
};

constexpr std::uint8_t port_select   = 0;
constexpr std::uint8_t port_deselect = 1;
constexpr std::uint8_t port_frame    = 2;
constexpr std::uint8_t port_ending   = 3;
constexpr std::uint8_t port_dropped  = 4;

struct spi_port_entry
{
  std::uint64_t                due_ps{};
  std::uint64_t                unit{};
  std::uint64_t                frame_ps{};
  std::uint64_t                posted_ps{};
  std::optional<std::uint64_t> wire_end_ps{};
  std::uint32_t                lane{};
  std::uint32_t                word{};
  std::uint8_t                 kind{};
  std::uint8_t                 bits{};

  friend auto operator==(spi_port_entry const&, spi_port_entry const&) -> bool = default;
};

struct spi_port
{
  std::vector<spi_port_entry>  entries;
  std::uint64_t                last_unit{};
  std::uint32_t                generation{};
  std::uint8_t                 level{};
  std::optional<std::uint64_t> reset_ps;

  friend auto operator==(spi_port const&, spi_port const&) -> bool = default;
};

}

template <> struct port_traits<spi_port_entry>
{
  static constexpr auto         span    = &spi_port_entry::frame_ps;
  static constexpr std::uint8_t stop    = port_deselect;
  static constexpr std::uint8_t running = port_frame;
  static constexpr auto         ends_at_due(std::uint8_t kind) -> bool
  {
    return kind == port_ending;
  }
  static constexpr std::string_view protocol = "SPI";
};

namespace
{
auto frame_start(std::span<spi_port_entry const> entries, std::uint64_t posted, std::uint64_t unit)
    -> std::uint64_t
{
  return entry_start(entries, posted, unit);
}

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
  std::uint32_t                       owed{};
  std::uint32_t                       reply{};
  std::vector<spi_completion>         completions;
  std::uint64_t                       refused{};
  spi_port                            port;

  friend auto operator==(spi_state const&, spi_state const&) -> bool = default;
};

struct spi_link_attachment
{
  std::shared_ptr<spi_link_target> device;
  gpio_port*                       cs{};
  std::uint8_t                     pin{};
  subscription                     listener;
  bool                             muted{};
};

/**
 * @brief SPI1 to SPI3 of the STM32U5 and SPI1 to SPI6 of the STM32H5, RM0456 section 68 and RM0481
 *        section 58 "Serial peripheral interface (SPI)",
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
 *        FIFOs of 16 bytes, 8 on the spi_variant's limited instances, SPI3 of the STM32U5 and SPI4
 *        to SPI6 of the STM32H5, RM0456 Table 696 and RM0481 Table 645, a frame taking 1 to 4
 *        bytes, packed by the access size. A 17 to 24-bit frame takes 3 FIFO bytes, as
 *        RM0456 68.4.12 "Data packet control" states. TSIZE frames per transfer, then EOT, CSTART
 *        cleared, and TXTF once TSIZE frames were written, which clears TXPIE and DXPIE. TSIZE 0
 *        runs until SPE clears. TXC copies EOT when TSIZE is set, and otherwise is set while the TX
 * FIFO and the shifter are empty. TXP, RXP, DXP by the FTHLV packet, TXP by the remaining frames
 * for the last packet, RXWNE, RXPLVL, CTSIZE except on the instances the spi_variant gives no
 *        counter, SPI3 of the STM32U5. `SPI_I2SCFGR` of the STM32H5 SPI1 to SPI3 is plain storage.
 * A full RX FIFO sets OVR and drops the frame, or with CR1.MASRX suspends the clock and sets SUSP
 * until IFCR.SUSPC. CSUSP stops after the current frame and sets SUSP. IFCR clears its flags,
 * re-enabling clears EOT, OVR, UDR and CRCE, the SPI IRQ follows SR and IER while SPE is set, EOTIE
 * covering EOT, SUSP and TXC, and only MODF with MODFIE while SPE is clear, RM0456 section 68.7. A
 * device is selected by its GPIO chip select pin, active low, or by the hardware SS output from
 * CSTART to the end of the transfer while CFG2.SSOE is set. With no device selected MISO reads all
 * ones, a pulled-up line. The devices are saved and loaded with the model. With CFG1.TXDMAEN the
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
 *        with the model. A reset of any kind drops its frames not yet heard, each reported
 *        through spi_controller_sink::dropped() with unit 0 on core::host_lane, at its end or at
 *        the reset's time when that is later.
 *
 *        Unit primitives, target mode: spi_controller_port::select(), frame(), deselect() and
 *        drop() queue entries on the lane they name, each with a unit above the last one. The port
 *        runs the entries due at one instruction count in unit order, a frame's end with its
 *        frame's unit, after the events of lower lanes, and each entry has its own lane event whose
 *        payload is the port generation above the lane. A due time in the past keeps its own
 *        instruction count. A frame starts at the latest of its time, the end of every frame queued
 *        before it and the time of every queued deselect(), a frame of a lower unit than a queued
 *        deselect() left out. A frame whose time is at or after that deselect()'s, as a link proxy
 *        posts it, gets the same start before and after the deselect() runs. The chip select stays
 *        at the level the last select() or deselect() gave between frames. A select() that lowers
 *        the level is an SS edge, synchronizing an enabled target as a transfer's start does. The
 *        level is a pin level: every reset keeps it, while the synchronization restarts with the
 *        fresh model, so a target re-enabled under a held select ignores frames until the next
 *        edge, RM0456 68.4.11. A deselect() drops every frame of a lower unit still queued or
 *        shifting, a frame cut by the rise neither lands nor counts, the model's rule. A dropped
 *        unit keeps its own report time. An RCC reset of the SPI drops the frames started at or
 *        before its time, re-armed at the later of their end and that time, and keeps the later
 *        frames, which run in the fresh model at their own starts, as the external master's SCK
 *        keeps running. Under a board controller, a frame dropped by that reset holds the wire
 *        until its end, so a frame queued after the reset starts at that end at the earliest. Under
 *        a link controller, each kept frame starts again from its own time and the entries still
 *        ahead of it, the dropped frames left out, since the master may have cut them, so it gets
 *        the start a frame queued at the reset would get, and one whose new start is at or before
 *        the reset is dropped the same way. Every frame entry saves its own time for this. A frame
 *        queued after that reset with a start at or before its time is dropped the same way, and a
 *        select(), deselect() or drop() queued after it with a time before it is due at it, so each
 *        entry has the outcome it would have had queued before the reset. A deselect() queued
 *        after that reset with a time before it counts at that time in the start of a frame queued
 *        after it, as queued before the reset. A frame an RCC reset dropped below a deselect() is
 *        reported at the later of its end and that reset, where queued before the reset the
 *        deselect() cuts it at the deselect's time. The model keeps the time of the last RCC reset
 *        until the next machine reset. A machine reset of any kind, a power cycle included, drops
 *        every frame, re-armed at their end less the local time of the reset, 0 at the earliest,
 *        which the model takes in before_reset(). Under a board controller each frame it drops
 *        holds the wire to that rebased end, and every wire end an RCC reset left is rebased the
 *        same way. Queued level entries and dropped units survive every reset at the same rebased
 *        times. Each dropped unit is reported once, through
 *        spi_controller_sink::dropped() from its lane event. The entries, the level, the
 *        generation, the last unit and the time of the last RCC reset are saved with the model.
 *
 *        Deferred replies, controller mode: the model exchanges each frame with
 *        spi_target::exchange_deferred() at the end of its shift, with the frame's generation as
 *        ticket. A device that leaves the reply pending completes it through
 *        spi_target_port::complete(), one event on the lane it names. A due time in the past keeps
 *        its own instruction count, and its time stays the one the device gave. The frame ends at
 *        the latest completion of every selected device, with the AND of every reply, and its RX
 *        FIFO entry, RXP, TXC, EOT, the hardware SS release, a CSUSP or MASRX suspension and the
 *        next frame follow that time. SCK stays idle until then. Clearing SPE or a reset of any
 *        kind drops the pending frame and moves the generation, so its later completions are
 *        refused when they fall due and counted in the saved `refused`. The pending frame and every
 *        completion not yet due are saved with the model. A completion event survives an RCC reset
 *        with its record, a machine reset clears both. Each device is attached to the model at its
 *        construction and detached at its destruction.
 *
 *        Link attachments, through spi_link_port: a link target is exchanged with after every
 *        board device, in attachment order, and its replies join the AND. It is selected at each
 *        frame end by the level gpio_port::level() reads on its chip select pin, or by the SS
 *        output, from CSTART to the end of the transfer while CFG2.SSOE is set. Its pin's edges
 *        and the SS output's edges reach it through spi_target::select(), and every machine reset
 *        and power cut reaches it as it reaches a board device. The SS release at the end of a
 *        transfer is stamped at the last frame's end, the other SS edges at the register write or
 *        reset that makes them. A proxy attached while its chip select is active hears
 *        select(true) at the attach. The model runs no mode check on it. A link controller drives
 *        a target-mode instance as a board controller does. The model's saved state holds the
 *        board attachments only, so a member snapshot has the same bytes with and without link
 *        attachments and their mute flags.
 *
 *        SPI_CFG1 and SPI_CFG2 are write-protected while CR1.SPE is set, RM0456 68.8.3 and 68.8.4,
 *        RM0481 58.11.3 and 58.11.4: a write then keeps the old value of every protected bit, for
 *        every initiator, and a firmware write that changes one logs one warning per instance,
 *        a model_error under strict_peripherals. On the STM32U5 TXDMAEN and RXDMAEN of SPI_CFG1
 *        stay writable. The model's own update of MASTER at a mode fault is outside the rule.
 *
 *        Not modelled, logged once: half duplex, CRC, TI mode, the autonomous trigger, the SS pulse
 *        and timing options.
 */
class spi final : public dispatch_model<spi, spi_state>,
                  public spi_controller_port,
                  public spi_target_port,
                  public reset_listener,
                  public bus_reset_listener,
                  public spi_link_port
{
public:
  spi(model_context&                       ctx,
      peripheral_desc const&               desc,
      std::vector<spi_attachment>          attachments,
      std::shared_ptr<spi_controller_sink> controller,
      spi_variant                          variant);
  spi(spi const&)                    = delete;
  auto operator=(spi const&) -> spi& = delete;
  spi(spi&&)                         = delete;
  auto operator=(spi&&) -> spi&      = delete;
  ~spi() override;

  void before_reset(reset_kind kind) override;
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
  void complete(std::uint64_t ticket, std::uint32_t word, vtime at, std::uint32_t lane) override;
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
  void attach_target(
      std::shared_ptr<spi_link_target> target,
      std::string                      cs_port,
      std::optional<std::uint8_t>      cs_pin) override;
  void detach_target(spi_link_target& target) noexcept override;
  void attach_controller(std::shared_ptr<spi_link_controller> controller) override;
  void detach_controller(spi_link_controller& controller) noexcept override;
  void mute(spi_link_target const& target) override;
  auto unmute(spi_link_target const& target) -> bool override;
  auto muted(spi_link_target const& target) const -> bool override;
  auto frame_time() const -> std::uint64_t override;
  auto board_use() const -> spi_board_use override;

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
  void               rethrow_refused();

  void               start_frame(std::uint64_t from_ps);
  void               finish_frame();
  void               land_frame();
  void               completion_due(std::uint64_t ticket);
  void               stop(bool completed, vtime at);
  void               hardware_select(bool selected);
  void               flags();
  void               warn_unsupported();
  void               disable();
  void               host_edge_reached();
  void               begin_host_frame();
  void               end_host_frame(std::uint8_t bits, std::uint32_t word);
  [[nodiscard]] auto host_selects() const -> bool;
  [[nodiscard]] auto nss_active() const -> bool;
  [[nodiscard]] auto port_due_ps(vtime at, std::uint32_t lane, std::uint64_t unit) const
      -> std::uint64_t;
  [[nodiscard]] auto after_model_reset(std::uint64_t due_ps) const -> std::uint64_t;
  void               queue_port(spi_port_entry entry);
  void               schedule_port(spi_port_entry const& entry);
  void               rearm_port(
      spi_port                              port,
      std::vector<spi_host_transfer> const& transfers,
      std::uint32_t                         host_next,
      std::optional<reset_kind>             kind);
  void               run_port(std::uint32_t lane);
  void               port_selected();
  void               port_deselected(spi_port_entry const& entry);
  [[nodiscard]] auto faults_on_select() const -> bool;
  [[nodiscard]] auto ss_output() const -> bool;
  [[nodiscard]] auto link_selected(spi_link_attachment const& link) const -> bool;
  [[nodiscard]] auto link_of(this auto& self, spi_link_target const& target) -> auto&;
  void               link_ss_changed(bool was, vtime at);

  register_file&                               regs_;
  std::shared_ptr<spi_controller_sink>         controller_;
  std::vector<spi_link_attachment>             links_;
  std::shared_ptr<spi_link_controller>         link_controller_;
  spi_attachments<spi_state>                   attachments_;
  bool                                         limited_;
  bool                                         counted_;
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
  reg_ref                                      config1_;
  reg_ref                                      config2_;
  std::uint32_t                                config1_kept_;
  std::vector<std::string>                     mismatched_;
  bool                                         unsupported_logged_{};
  bool                                         no_clock_logged_{};
  bool                                         size_logged_{};
  bool                                         protect_logged_{};
  std::optional<std::uint64_t>                 reset_at_;
};

constexpr std::array<write_entry<spi>, 10> spi_writes{
    {
     {"SPI_CR1", &spi::control_written},
     {"SPI_CR2", &spi::plain_written},
     {"SPI_CFG1", &spi::plain_written},
     {"SPI_CFG2", nullptr},
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
    std::shared_ptr<spi_controller_sink> controller,
    spi_variant                          variant)
    : dispatch_model{ctx, desc, "SPI", spi_writes}, regs_{ctx.regs},
      controller_{std::move(controller)}, attachments_{state_, std::move(attachments)},
      limited_{std::ranges::contains(variant.limited, std::string_view{desc.name})},
      counted_{not limited_ or variant.limited_counter}, fifo_bytes_{limited_ ? 8u : 16u},
      status_{reg("SPI_SR")}, clear_{reg("SPI_IFCR")}, transmit_{reg("SPI_TXDR")},
      receive_{reg("SPI_RXDR")}, enable_{field("SPI_CR1", "SPE")},
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
      rx_dma_{ctx.peers, lower(desc.name) + "_rx_dma"}, mode_fault_flag_{field("SPI_SR", "MODF")},
      software_select_{field("SPI_CFG2", "SSM")}, select_input_{field("SPI_CR1", "SSI")},
      select_polarity_{field("SPI_CFG2", "SSIOP")}, underrun_{field("SPI_SR", "UDR")},
      underrun_config_{field("SPI_CFG1", "UDRCFG")}, underrun_data_{field("SPI_UDRDR", "UDRDR")},
      lsb_first_{field("SPI_CFG2", "LSBFRST")}, polarity_{field("SPI_CFG2", "CPOL")},
      phase_{field("SPI_CFG2", "CPHA")}, config1_{reg("SPI_CFG1")}, config2_{reg("SPI_CFG2")},
      config1_kept_{
          variant.dma_enables_writable ? ~(tx_dma_enable_.mask() | rx_dma_enable_.mask())
                                       : ~std::uint32_t{}}
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
  for(auto const& [reg_name, name] : std::array<std::pair<std::string_view, std::string_view>, 4>{
          {
           {"SPI_CFG1", "CRCEN"},
           {"SPI_CFG2", "SP"},
           {"SPI_CFG2", "MIDI"},
           {"SPI_CFG2", "MSSI"},
           }
  })
  {
    unsupported_.push_back(field(reg_name, name));
  }
  if(variant.autocr)
  {
    unsupported_.push_back(field("SPI_AUTOCR", "TRIGEN"));
  }
  std::size_t attached = 0;
  try
  {
    for(; attached < attachments_.size(); ++attached)
    {
      attachments_[attached].device->attached(*this);
    }
  }
  catch(...)
  {
    while(attached > 0)
    {
      attachments_[--attached].device->detached();
    }
    throw;
  }
}

spi::~spi()
{
  for(auto const& attached : attachments_)
  {
    attached.device->detached();
  }
  for(auto const& link : links_)
  {
    link.device->detached();
  }
  if(link_controller_)
  {
    link_controller_->unlinked();
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
  return spi_mask(frame_bits());
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
  return get(regs_, lsb_first_) != 0 ? spi_reversed(frame, frame_bits()) : frame;
}

auto spi::bus_mode() const -> std::uint8_t
{
  return static_cast<std::uint8_t>(get(regs_, polarity_) << 1 | get(regs_, phase_));
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

auto spi::board_use() const -> spi_board_use
{
  return {
      .controller = controller_ != nullptr and link_controller_ == nullptr,
      .devices    = not attachments_.empty()};
}

auto spi::frame_time() const -> std::uint64_t
{
  auto const hz  = ctx_.clock.hz(kernel_);
  auto const div = get(regs_, bypass_) != 0 ? 1u : 2u << get(regs_, baud_);
  return hz == 0 ? 0 : std::uint64_t{frame_bits()} * div * ps_per_second / hz;
}

auto spi::frame_ps() -> std::uint64_t
{
  auto const ps = frame_time();
  if(ps == 0 and not std::exchange(no_clock_logged_, true))
  {
    warn("SPI_CFG1", "the SPI kernel clock is stopped: no frame is transferred");
  }
  return ps;
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

void spi::before_reset(reset_kind)
{
  reset_at_ = ctx_.clock.time().ps;
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
  auto const ss_was = ss_output();
  for(auto const& name : claims())
  {
    regs_.poke(reg(name).offset, describe(name).reset);
  }
  auto const generation      = state_.generation + 1;
  auto const host_generation = state_.host_generation + (state_.host.empty() ? 0u : 1u);
  auto       selected        = state_.selected;
  auto       completions     = kind ? std::vector<spi_completion>{} : state_.completions;
  auto const refused         = state_.refused;
  auto       port            = std::move(state_.port);
  auto const transfers       = std::move(state_.host);
  auto const host_next       = state_.host_next;
  state_                     = {};
  state_.generation          = generation;
  state_.host_generation     = host_generation;
  state_.completions         = std::move(completions);
  state_.refused             = refused;
  selected.resize(attachments_.size());
  state_.selected = selected;
  state_.mode_checked.resize(attachments_.size());
  rearm_port(std::move(port), transfers, host_next, kind);
  if(kind)
  {
    for(auto const& attached : attachments_)
    {
      attached.device->reset(*kind);
    }
    for(auto const& link : links_)
    {
      link.device->reset(*kind);
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
  link_ss_changed(ss_was, ctx_.clock.since_power_on());
  attachments_.subscribe(ctx_, desc_.name);
  tx_dma_.resolve();
  rx_dma_.resolve();
  flags();
}

void spi::power_cut()
{
  auto const         at = ctx_.clock.since_power_on();
  std::exception_ptr first;
  auto const         cut = [&](spi_target& device) {
    try
    {
      device.power_cut(at);
    }
    catch(...)
    {
      if(not first)
      {
        first = std::current_exception();
      }
    }
  };
  for(auto const& attached : attachments_)
  {
    cut(*attached.device);
  }
  for(auto const& link : links_)
  {
    cut(*link.device);
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
  auto const at   = vtime{state_.frame_end_ps + power_on_offset()};
  auto const out  = wire_frame(state_.shift);
  auto       in   = frame_mask();
  auto       owed = std::uint32_t{};
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
        ++owed;
      }
    }
  }
  for(auto const& link : links_)
  {
    if(link_selected(link))
    {
      auto const reply = link.device->exchange_deferred(
          out, static_cast<std::uint8_t>(frame_bits()), at, state_.generation);
      if(reply)
      {
        in &= *reply;
      }
      else
      {
        ++owed;
      }
    }
  }
  state_.reply = in;
  state_.owed  = owed;
  if(owed == 0)
  {
    land_frame();
  }
}

void spi::completion_due(std::uint64_t ticket)
{
  auto of_ticket = state_.completions | std::views::filter([ticket](spi_completion const& record) {
                     return record.ticket == ticket;
                   });
  auto const found = std::ranges::min_element(of_ticket, {}, &spi_completion::at_ps);
  if(found == of_ticket.end() or state_.owed == 0 or ticket != state_.generation)
  {
    ++state_.refused;
    if(found != of_ticket.end())
    {
      state_.completions.erase(found.base());
    }
    return;
  }
  auto const done = *found;
  state_.completions.erase(found.base());
  state_.reply &= done.word;
  state_.frame_end_ps = std::max(state_.frame_end_ps, done.at_ps);
  if(--state_.owed == 0)
  {
    land_frame();
  }
}

void spi::land_frame()
{
  state_.busy   = 0;
  auto const in = std::exchange(state_.reply, 0);
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
    stop(true, vtime{state_.frame_end_ps + power_on_offset()});
    return;
  }
  if(state_.suspending != 0)
  {
    state_.suspending = 0;
    update(regs_, suspended_, 1);
    stop(false, vtime{state_.frame_end_ps + power_on_offset()});
    return;
  }
  start_frame(state_.frame_end_ps);
}

void spi::stop(bool completed, vtime at)
{
  auto const ss_was     = ss_output();
  state_.running        = 0;
  state_.auto_suspended = 0;
  update(regs_, start_, 0);
  if(completed)
  {
    update(regs_, end_, 1);
  }
  hardware_select(false);
  link_ss_changed(ss_was, at);
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
  update(regs_, remaining_, counted_ and size > state_.done ? size - state_.done : 0);
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
  state_.owed        = 0;
  state_.reply       = 0;
  state_.suspending  = 0;
  state_.done        = 0;
  state_.written     = 0;
  state_.host_synced = 0;
  state_.host_taking = 0;
  state_.host_shift  = ~std::uint32_t{};
  stop(false, ctx_.clock.since_power_on());
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
    if(nss_active() and faults_on_select())
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
        stop(false, ctx_.clock.since_power_on());
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
    auto const ss_was = ss_output();
    state_.running    = 1;
    state_.done       = 0;
    std::ranges::fill(state_.mode_checked, 0);
    hardware_select(true);
    link_ss_changed(ss_was, ctx_.clock.since_power_on());
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
  auto const word = offset & ~3u;
  auto const protected_ =
      (word == config1_.offset or word == config2_.offset) and get(regs_, enable_) != 0;
  auto const before = regs_.peek(word);
  auto const result = dispatch_model::on_write(regs, offset, size, old, value, access);
  auto const now    = regs_.peek(word);
  auto const kept   = word == config1_.offset ? config1_kept_ : ~std::uint32_t{};
  if(protected_ and ((now ^ before) & kept) != 0)
  {
    regs_.poke(word, (now & ~kept) | (before & kept));
    if(access.by != initiator::debugger)
    {
      auto const name = word == config1_.offset ? "SPI_CFG1" : "SPI_CFG2";
      if(not std::exchange(protect_logged_, true))
      {
        warn(
            name, std::format(
                      "{} {} written while SPE = 1, the old value of its write-protected bits is "
                      "kept, RM0456 68.8.3 and 68.8.4, RM0481 58.11.3 and 58.11.4",
                      desc_.name, name));
      }
      if(ctx_.strict_peripherals)
      {
        throw model_error(desc_.name, name, word);
      }
    }
  }
  flags();
  return result;
}

void spi::on_event(std::uint32_t kind, std::uint64_t payload)
{
  if(kind == port_due)
  {
    if(payload >> 32 == state_.port.generation)
    {
      run_port(static_cast<std::uint32_t>(payload));
      rethrow_refused();
    }
    return;
  }
  if(kind == completed)
  {
    completion_due(payload);
    flags();
    rethrow_refused();
    return;
  }
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
  auto const idle = state_.host.empty();
  auto const from = queue_host_transfer(
      state_.host, ctx_.part.part, desc_.name, ctx_.clock.time().ps, start, frame_ps, bits, frames,
      [&] {
        if(state_.port.level != 0
           or std::ranges::any_of(
               state_.port.entries, [](auto const& entry) { return entry.kind != port_dropped; }))
        {
          throw std::invalid_argument(
              "an SPI transfer waits until the port entries ran and the chip select is released");
        }
      });
  if(idle)
  {
    ctx_.events.schedule_at(from, host_edge, state_.host_generation, core::host_lane, ctx_.clock);
  }
  return from;
}

void spi::complete(std::uint64_t ticket, std::uint32_t word, vtime at, std::uint32_t lane)
{
  if(lane == 0)
  {
    throw std::invalid_argument("an SPI completion goes on a lane of 1 and up");
  }
  auto const offset = power_on_offset();
  auto const since  = lane_due_ps(at.ps, offset);
  if(not since)
  {
    throw std::invalid_argument(
        std::format(
            "an SPI completion at {} ps since power-on falls before the last reset at {} ps", at.ps,
            offset));
  }
  auto const due = *since;
  state_.completions.push_back({.ticket = ticket, .word = word, .at_ps = due});
  ctx_.events.schedule_at(vtime{due}, completed, ticket, lane, ctx_.clock);
}

auto spi::port_due_ps(vtime at, std::uint32_t lane, std::uint64_t unit) const -> std::uint64_t
{
  check_port_entry(ctx_, desc_.name, controller_ != nullptr, state_.port, max_frames, lane, unit);
  if(not state_.host.empty())
  {
    throw std::invalid_argument("an SPI port entry waits until the queued transfers are done");
  }
  return at.ps;
}

void spi::schedule_port(spi_port_entry const& entry)
{
  stm32::schedule_port(ctx_, port_due, state_.port, entry);
}

void spi::queue_port(spi_port_entry entry)
{
  stm32::queue_port(ctx_, port_due, state_.port, entry);
}

auto spi::after_model_reset(std::uint64_t due_ps) const -> std::uint64_t
{
  return std::max(due_ps, state_.port.reset_ps.value_or(0));
}

void spi::select(vtime at, std::uint32_t lane, std::uint64_t unit)
{
  auto const posted = port_due_ps(at, lane, unit);
  queue_port(
      {.due_ps    = after_model_reset(posted),
       .unit      = unit,
       .posted_ps = posted,
       .lane      = lane,
       .kind      = port_select});
}

void spi::deselect(vtime at, std::uint32_t lane, std::uint64_t unit)
{
  auto const posted = port_due_ps(at, lane, unit);
  queue_port(
      {.due_ps    = after_model_reset(posted),
       .unit      = unit,
       .posted_ps = posted,
       .lane      = lane,
       .kind      = port_deselect});
}

void spi::drop(vtime at, std::uint32_t lane, std::uint64_t unit)
{
  queue_port(
      {.due_ps = after_model_reset(port_due_ps(at, lane, unit)),
       .unit   = unit,
       .lane   = lane,
       .word   = ~std::uint32_t{},
       .kind   = port_dropped});
}

void spi::frame(
    std::uint32_t word,
    std::uint8_t  bits,
    std::uint64_t frame_ps,
    vtime         at,
    std::uint32_t lane,
    std::uint64_t unit)
{
  auto const due = port_due_ps(at, lane, unit);
  if(frame_ps == 0 or bits < 4 or bits > 32 or (word & ~spi_mask(bits)) != 0)
  {
    throw std::invalid_argument(
        "an SPI port frame has 4 to 32 bits, a word below 2**bits and at least 1 ps");
  }
  auto const last_reset = state_.port.reset_ps;
  auto const start      = frame_start(state_.port.entries, due, unit);
  if(frame_ps > std::numeric_limits<std::uint64_t>::max() - start)
  {
    throw std::invalid_argument("an SPI port frame ends within the 64-bit picosecond range");
  }
  if(last_reset and start <= *last_reset)
  {
    queue_port(
        {.due_ps      = std::max(*last_reset, start + frame_ps),
         .unit        = unit,
         .frame_ps    = frame_ps,
         .posted_ps   = due,
         .wire_end_ps = link_controller_ ? std::nullopt : std::optional{start + frame_ps},
         .lane        = lane,
         .word        = spi_mask(bits),
         .kind        = port_dropped,
         .bits        = bits});
    return;
  }
  queue_port(
      {.due_ps    = start,
       .unit      = unit,
       .frame_ps  = frame_ps,
       .posted_ps = due,
       .lane      = lane,
       .word      = word,
       .kind      = port_frame,
       .bits      = bits});
}

void spi::port_selected()
{
  if(std::exchange(state_.port.level, 1) != 0)
  {
    return;
  }
  state_.host_mode_checked = 0;
  if(get(regs_, enable_) != 0 and faults_on_select())
  {
    update(regs_, mode_fault_flag_, 1);
    update(regs_, enable_, 0);
    update(regs_, master_, 0);
    disable();
  }
  state_.host_synced = get(regs_, enable_) != 0 and get(regs_, master_) == 0 ? 1 : 0;
}

void spi::port_deselected(spi_port_entry const& entry)
{
  state_.port.level  = 0;
  state_.host_synced = 0;
  auto const cut     = [&](spi_port_entry const& queued) {
    return (queued.kind == port_frame or queued.kind == port_ending) and queued.unit < entry.unit;
  };
  std::vector<std::pair<std::uint64_t, std::uint32_t>> reports;
  for(auto const& queued : state_.port.entries)
  {
    if(not cut(queued))
    {
      continue;
    }
    if(queued.kind == port_ending)
    {
      state_.host_busy   = 0;
      state_.host_taking = 0;
      state_.host_shift  = ~std::uint32_t{};
    }
    reports.emplace_back(queued.unit, spi_mask(queued.bits));
  }
  std::erase_if(state_.port.entries, cut);
  std::ranges::sort(reports);
  flags();
  for(auto const& [unit, sent] : reports)
  {
    controller_->dropped(unit, sent, vtime{entry.due_ps});
  }
}

void spi::run_port(std::uint32_t lane)
{
  auto const now = ctx_.clock.time().ps;
  for(;;)
  {
    auto&      entries = state_.port.entries;
    auto const pick    = next_due<spi_port_entry>(
        entries, lane, now, ctx_.clock, [](spi_port_entry const&) { return false; });
    if(not pick)
    {
      return;
    }
    auto const entry = entries[*pick];
    switch(entry.kind)
    {
      case port_select:
        entries.erase(entries.begin() + static_cast<std::ptrdiff_t>(*pick));
        port_selected();
        flags();
        break;
      case port_deselect:
        entries.erase(entries.begin() + static_cast<std::ptrdiff_t>(*pick));
        port_deselected(entry);
        break;
      case port_frame:
        entries[*pick].kind   = port_ending;
        entries[*pick].due_ps = entry.due_ps + entry.frame_ps;
        schedule_port(entries[*pick]);
        begin_host_frame();
        flags();
        break;
      case port_ending:
      {
        entries.erase(entries.begin() + static_cast<std::ptrdiff_t>(*pick));
        auto const received = state_.host_shift & spi_mask(entry.bits);
        end_host_frame(entry.bits, entry.word);
        flags();
        if(link_controller_)
        {
          link_controller_->unit_frame(entry.unit, entry.word, received, vtime{entry.due_ps});
        }
        else
        {
          controller_->frame(entry.word, received, vtime{entry.due_ps});
        }
        break;
      }
      default:
        entries.erase(entries.begin() + static_cast<std::ptrdiff_t>(*pick));
        flags();
        controller_->dropped(entry.unit, entry.word, vtime{entry.due_ps});
        break;
    }
  }
}

void spi::rearm_port(
    spi_port                              port,
    std::vector<spi_host_transfer> const& transfers,
    std::uint32_t                         host_next,
    std::optional<reset_kind>             kind)
{
  auto const now   = ctx_.clock.time().ps;
  auto const shift = kind ? std::exchange(reset_at_, std::nullopt).value_or(0) : 0;
  auto const moved = [&](std::uint64_t due) {
    return std::max(now, due > shift ? due - shift : 0);
  };
  auto const restart = not kind and link_controller_;
  for(auto& entry : port.entries)
  {
    if(kind and entry.wire_end_ps)
    {
      entry.wire_end_ps = moved(*entry.wire_end_ps);
    }
    if(entry.kind == port_frame and (kind or (not restart and entry.due_ps <= now)))
    {
      entry.due_ps += entry.frame_ps;
      entry.kind = port_ending;
    }
    if(entry.kind == port_ending)
    {
      if(not link_controller_)
      {
        entry.wire_end_ps = kind ? moved(entry.due_ps) : entry.due_ps;
      }
      entry.due_ps = moved(entry.due_ps);
      entry.word   = spi_mask(entry.bits);
      entry.kind   = port_dropped;
    }
    else if(entry.kind != port_frame)
    {
      entry.due_ps = moved(entry.due_ps);
      if(kind)
      {
        entry.posted_ps = entry.due_ps;
      }
    }
  }
  if(restart)
  {
    for(auto& entry : port.entries)
    {
      if(entry.kind != port_frame)
      {
        continue;
      }
      auto const start = frame_start(port.entries, entry.posted_ps, entry.unit);
      if(start <= now)
      {
        entry.due_ps = std::max(now, start + entry.frame_ps);
        entry.word   = spi_mask(entry.bits);
        entry.kind   = port_dropped;
      }
      else
      {
        entry.due_ps = start;
      }
    }
  }
  for(std::size_t index = 0; index < transfers.size(); ++index)
  {
    auto const& queued = transfers[index];
    for(std::size_t frame = index == 0 ? host_next : 0; frame < queued.frames.size(); ++frame)
    {
      port.entries.push_back(
          {.due_ps = moved(queued.start_ps + (frame + 1) * queued.frame_ps),
           .lane   = core::host_lane,
           .word   = spi_mask(queued.bits),
           .kind   = port_dropped,
           .bits   = queued.bits});
    }
  }
  port.reset_ps = kind ? std::nullopt : std::optional{now};
  ++port.generation;
  state_.port = std::move(port);
  for(auto const& entry : state_.port.entries)
  {
    schedule_port(entry);
  }
}

auto spi::ss_output() const -> bool
{
  return state_.running != 0 and get(regs_, ss_output_) != 0;
}

auto spi::link_selected(spi_link_attachment const& link) const -> bool
{
  return link.cs != nullptr ? not link.cs->level(link.pin) : ss_output();
}

void spi::link_ss_changed(bool was, vtime at)
{
  auto const now = ss_output();
  if(now == was)
  {
    return;
  }
  for(auto const& link : links_)
  {
    if(link.cs == nullptr)
    {
      link.device->select(now, at);
    }
  }
}

auto spi::link_of(this auto& self, spi_link_target const& target) -> auto&
{
  auto const found = std::ranges::find(
      self.links_, &target, [](spi_link_attachment const& link) { return link.device.get(); });
  if(found == self.links_.end())
  {
    throw std::invalid_argument(
        std::format("the device is not a link target attached to {}", self.desc_.name));
  }
  return *found;
}

void spi::attach_target(
    std::shared_ptr<spi_link_target> target,
    std::string                      cs_port,
    std::optional<std::uint8_t>      cs_pin)
{
  if(not target)
  {
    throw std::invalid_argument("an SPI link target is a device");
  }
  if(std::ranges::contains(
         links_, target.get(), [](spi_link_attachment const& link) { return link.device.get(); }))
  {
    throw std::invalid_argument(
        std::format("the link target is attached to {} already", desc_.name));
  }
  if(controller_)
  {
    throw config_error(
        ctx_.part.part, 0, std::format("links.{}", desc_.name),
        "an SPI instance with no controller, for a link target");
  }
  auto* const cs = cs_pin ? dynamic_cast<gpio_port*>(ctx_.peers.find(cs_port)) : nullptr;
  if(cs_pin and cs == nullptr)
  {
    throw config_error(
        ctx_.part.part, 0, std::format("links.{}.cs_port", desc_.name),
        std::format("a GPIO port the chip select of a {} link target names", desc_.name));
  }
  if(cs_pin and *cs_pin >= cs->pin_count())
  {
    throw std::invalid_argument(std::format("{} has no chip select pin {}", cs_port, *cs_pin));
  }
  spi_link_attachment link{
      .device = target, .cs = cs, .pin = cs_pin.value_or(0), .listener = {}, .muted = false};
  if(cs != nullptr)
  {
    link.listener = cs->on_change(
        [this, device = target.get(), pin = *cs_pin](std::uint8_t changed, bool level) {
          if(changed == pin)
          {
            device->select(not level, ctx_.clock.since_power_on());
          }
        });
  }
  links_.push_back(std::move(link));
  auto const by_device = [](spi_link_attachment const& one) { return one.device.get(); };
  try
  {
    target->linked(*this, ctx_.clock);
    target->attached(*this);
  }
  catch(...)
  {
    std::erase_if(links_, [&](spi_link_attachment const& one) { return one.device == target; });
    throw;
  }
  auto const found = std::ranges::find(links_, target.get(), by_device);
  if(found != links_.end() and link_selected(*found))
  {
    target->select(true, ctx_.clock.since_power_on());
  }
}

void spi::detach_target(spi_link_target& target) noexcept
{
  auto const found = std::ranges::find(
      links_, &target, [](spi_link_attachment const& link) { return link.device.get(); });
  if(found == links_.end())
  {
    return;
  }
  auto const device = found->device;
  device->detached();
  links_.erase(std::ranges::find(links_, device.get(), [](spi_link_attachment const& link) {
    return link.device.get();
  }));
}

void spi::attach_controller(std::shared_ptr<spi_link_controller> controller)
{
  if(not controller)
  {
    throw std::invalid_argument("an SPI link controller is a controller");
  }
  if(link_controller_)
  {
    throw std::invalid_argument(
        std::format("a link controller is attached to {} already", desc_.name));
  }
  if(controller_ or not attachments_.empty() or not links_.empty())
  {
    throw config_error(
        ctx_.part.part, 0, std::format("links.{}", desc_.name),
        "an SPI instance with no board controller, no board device and no link target, for a "
        "link controller");
  }
  auto const last_unit = std::exchange(state_.port.last_unit, 0);
  auto const level     = std::exchange(state_.port.level, 0);
  controller_          = controller;
  link_controller_     = controller;
  try
  {
    controller->linked(*this, ctx_.clock);
  }
  catch(...)
  {
    controller_.reset();
    link_controller_.reset();
    state_.port.last_unit = last_unit;
    state_.port.level     = level;
    throw;
  }
}

void spi::detach_controller(spi_link_controller& controller) noexcept
{
  if(link_controller_.get() != &controller)
  {
    return;
  }
  auto& port         = state_.port;
  state_.host_busy   = 0;
  state_.host_taking = 0;
  state_.host_shift  = ~std::uint32_t{};
  if(not port.entries.empty())
  {
    port.entries.clear();
    ++port.generation;
  }
  if(not state_.host.empty())
  {
    state_.host.clear();
    state_.host_next = 0;
    ++state_.host_generation;
  }
  port.last_unit       = 0;
  port.level           = 0;
  state_.host_selected = 0;
  state_.host_synced   = 0;
  controller_.reset();
  auto const gone = std::move(link_controller_);
  flags();
  gone->unlinked();
}

void spi::mute(spi_link_target const& target)
{
  link_of(target).muted = true;
}

auto spi::unmute(spi_link_target const& target) -> bool
{
  auto& link = link_of(target);
  link.muted = false;
  return not link_selected(link);
}

auto spi::muted(spi_link_target const& target) const -> bool
{
  return link_of(target).muted;
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
  return nss_active() and state_.host_synced != 0;
}

auto spi::nss_active() const -> bool
{
  return state_.host_selected != 0 or state_.port.level != 0;
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
    spi_check_mode(mismatched_, bus_mode(), controller_->mode(), [] { return "the controller"; });
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

void spi::end_host_frame(std::uint8_t bits, std::uint32_t word)
{
  state_.host_busy = 0;
  if(state_.host_taking == 0)
  {
    return;
  }
  if(bits != frame_bits() and not std::exchange(size_logged_, true))
  {
    warn(
        "SPI_CFG1",
        std::format(
            "the SPI controller sends {}-bit frames to a {}-bit DSIZE: each frame lands masked to "
            "DSIZE + 1 bits",
            bits, frame_bits()));
  }
  auto const in        = wire_frame(word);
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
    end_host_frame(now.bits, now.frames[state_.host_next]);
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
  attachments_.save_devices(copy.devices);
  reflex::serde::bson::serializer{out}.dump(copy);
}

void spi::load(std::span<std::byte const> in)
{
  auto loaded = reflex::serde::bson::deserializer{in}.template load<spi_state>();
  if(not controller_ and (not loaded.port.entries.empty() or not loaded.host.empty()))
  {
    throw config_error(
        ctx_.part.part, 0, std::format("devices.{}", desc_.name),
        "an SPI controller attached to the instance, the snapshot holds port entries or queued "
        "transfers");
  }
  state_ = std::move(loaded);
  attachments_.load_devices(ctx_, desc_.name);
}
}

spi_link_target::~spi_link_target()         = default;
spi_link_controller::~spi_link_controller() = default;

void spi_link_controller::unit_frame(
    std::uint64_t,
    std::uint32_t sent,
    std::uint32_t received,
    vtime         at)
{
  frame(sent, received, at);
}

spi_link_port::~spi_link_port() = default;

auto spi_factory(
    decltype(board::spi) const&             devices_,
    decltype(board::spi_controllers) const& controllers_,
    spi_variant                             variant) -> model_factory
{
  return [devices = devices_, controllers = controllers_,
          variant](model_context& ctx, peripheral_desc const& desc) -> std::shared_ptr<model> {
    auto const found      = devices.find(desc.name);
    auto const controller = controllers.find(desc.name);
    return std::make_shared<spi>(
        ctx, desc, found != devices.end() ? found->second : std::vector<spi_attachment>{},
        controller != controllers.end() ? controller->second : nullptr, variant);
  };
}

namespace
{
constexpr auto spi_row_table = std::to_array<builtin>({
    row_of<^^spi_factory, ^^u5_spi, ^^board::spi, ^^board::spi_controllers>(
        {.ip = "SPI", .ip_version_glob = "spi2s3_v1*", .part_globs = u5_parts}),
    row_of<^^spi_factory, ^^h5_spi, ^^board::spi, ^^board::spi_controllers>(
        {.ip = "SPI", .ip_version_glob = "spi2s3_v2*", .part_globs = h5_parts}),
    row_of<^^spi_classic_factory, ^^l1_spi_ti, ^^board::spi, ^^board::spi_controllers>(
        {.ip = "SPI", .ip_version_glob = "spi2s1_v2_*", .part_globs = l1_parts}),
    row_of<^^spi_classic_factory, ^^l1_spi, ^^board::spi, ^^board::spi_controllers>(
        {.ip = "SPI", .ip_version_glob = "spi2s1_v1_*", .part_globs = l1_parts}),
    row_of<^^spi_classic_factory, ^^l1_spi, ^^board::spi, ^^board::spi_controllers>(
        {.ip = "SPI", .ip_version_glob = "STM32L1_spi_cat2*", .part_globs = l1_parts}),
    row_of<^^spi_classic_factory, ^^l4_spi, ^^board::spi, ^^board::spi_controllers>(
        {.ip = "SPI", .ip_version_glob = "spi2s1_v3_*", .part_globs = l4_parts}),
});
}

auto spi_rows() -> std::span<builtin const>
{
  return spi_row_table;
}
}
