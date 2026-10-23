#include "factories.hpp"
#include "model_base.hpp"

#include <popcorn/core/errors.hpp>
#include <popcorn/periph/stm32/usb.hpp>
#include <popcorn/periph/usb.hpp>
#include <popcorn/periph/usb_host.hpp>

#include <algorithm>
#include <array>
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
usb_host_port::~usb_host_port() = default;

namespace
{
constexpr std::size_t   endpoints        = 6;
constexpr std::uint32_t enumeration_done = 0;
constexpr std::uint32_t periodic_end     = 1;
constexpr std::uint32_t host_wake        = 2;
constexpr std::uint32_t idle_suspend     = 3;
constexpr std::uint64_t ps_per_us        = 1'000'000;
constexpr std::uint64_t enumeration_ps   = 10 * ps_per_us;
constexpr std::uint64_t frame_ps         = 1000 * ps_per_us;
constexpr std::uint64_t idle_ps          = 3 * frame_ps;
constexpr std::uint32_t all_tx_fifos     = 0x10;
constexpr std::uint32_t full_speed_48mhz = 3;
constexpr std::uint32_t endpoint_stride  = 0x20;
constexpr std::uint32_t window_stride    = 0x1000;

constexpr std::array<std::uint64_t, 4> periodic_percent{80, 85, 90, 95};
constexpr std::array<std::uint32_t, 4> ep0_packet{64, 32, 16, 8};

constexpr std::uint32_t pktsts_global_out_nak = 1;
constexpr std::uint32_t pktsts_out_data       = 2;
constexpr std::uint32_t pktsts_out_done       = 3;
constexpr std::uint32_t pktsts_setup_done     = 4;
constexpr std::uint32_t pktsts_setup_data     = 6;
constexpr std::uint8_t  set_address           = 5;
constexpr std::uint32_t isochronous           = 1;
constexpr std::size_t   fifo_ram_words        = 320;

enum warning : std::uint32_t
{
  host_mode        = 1u << 0,
  otg_negotiation  = 1u << 1,
  battery_charging = 1u << 2,
  test_mode        = 1u << 3,
  shared_tx_fifo   = 1u << 4,
  tx_overflow      = 1u << 5,
  rx_unread        = 1u << 6,
  rx_empty         = 1u << 7,
  babble           = 1u << 8,
  setup_overflow   = 1u << 9,
  fifo_ram         = 1u << 10,
  iso_dropped      = 1u << 11,
  iso_packets      = 1u << 12,
};

struct rx_packet
{
  std::uint32_t              status{};
  std::vector<std::uint32_t> words;

  friend auto operator==(rx_packet const&, rx_packet const&) -> bool = default;
};

struct endpoint_state
{
  std::uint8_t toggle{};
  bool         served{};

  friend auto operator==(endpoint_state const&, endpoint_state const&) -> bool = default;
};

struct otg_fs_state
{
  std::array<std::uint32_t, 4>                      generation{};
  std::vector<rx_packet>                            rx_fifo;
  std::vector<std::uint32_t>                        rx_data;
  std::array<std::vector<std::uint32_t>, endpoints> tx_fifo{};
  std::array<std::vector<std::uint16_t>, endpoints> tx_packets{};
  std::vector<std::byte>                            host;
  std::uint64_t                                     wake_ps{};
  std::array<endpoint_state, endpoints>             in{};
  std::array<endpoint_state, endpoints>             out{};
  std::uint16_t                                     frame{};
  std::uint8_t                                      address{};
  bool                                              address_pending{};
  bool                                              b_valid{};
  bool                                              pulled_up{};
  bool                                              cable{};
  bool                                              suspended{};

  friend auto operator==(otg_fs_state const&, otg_fs_state const&) -> bool = default;
};

struct in_fields
{
  field_ref                type;
  field_ref                packet_size;
  field_ref                active;
  field_ref                nak;
  field_ref                stall;
  field_ref                fifo_number;
  field_ref                clear_nak;
  field_ref                set_nak;
  std::optional<field_ref> set_data0;
  std::optional<field_ref> set_data1;
  std::optional<field_ref> data_pid;
  field_ref                disable;
  field_ref                enable;
  field_ref                complete;
  field_ref                disabled;
  field_ref                nak_effective;
  field_ref                fifo_empty;
  field_ref                size;
  field_ref                packets;
  field_ref                available;
  field_ref                depth;
};

struct out_fields
{
  field_ref                type;
  field_ref                active;
  field_ref                packet_size;
  field_ref                nak;
  field_ref                stall;
  field_ref                clear_nak;
  field_ref                set_nak;
  std::optional<field_ref> set_data0;
  std::optional<field_ref> set_data1;
  std::optional<field_ref> data_pid;
  field_ref                disable;
  field_ref                enable;
  field_ref                complete;
  field_ref                disabled;
  field_ref                setup_done;
  field_ref                size;
  field_ref                packets;
  std::optional<field_ref> setup_count;
};

[[nodiscard]] auto status_word(
    std::uint32_t endpoint,
    std::size_t   bytes,
    std::uint8_t  toggle,
    std::uint32_t pktsts,
    std::uint16_t frame) -> std::uint32_t
{
  auto const dpid = toggle != 0 ? 2u : 0u;
  return (endpoint & 0xfu)
       | ((static_cast<std::uint32_t>(bytes) & 0x7ffu) << 4)
       | (dpid << 15)
       | ((pktsts & 0xfu) << 17)
       | ((static_cast<std::uint32_t>(frame) & 0xfu) << 21);
}

[[nodiscard]] auto words_for(std::size_t bytes) -> std::size_t
{
  return (bytes + 3) / 4;
}

[[nodiscard]] auto refused(usb::handshake hs) -> usb::in_result
{
  return {.hs = hs, .pid = usb::data_pid::data0, .data = {}};
}

[[nodiscard]] auto pack(std::span<std::uint8_t const> bytes) -> std::vector<std::uint32_t>
{
  std::vector<std::uint32_t> words(words_for(bytes.size()));
  for(std::size_t index = 0; index < bytes.size(); ++index)
  {
    words[index / 4] |= std::uint32_t{bytes[index]} << (8 * (index % 4));
  }
  return words;
}

[[nodiscard]] auto pktsts_of(std::uint32_t status) -> std::uint32_t
{
  return (status >> 17) & 0xfu;
}

/**
 * @brief DWC2 OTG_FS core of the STM32U5 in device mode, slave FIFO mode, RM0456 section 72,
 *        and the bus side of the device as a usb::device_port.
 *
 * Core resets: CSRST, PSRST, FSRST, RXFFLSH and TXFFLSH read 0 again as soon as they are written,
 * AHBIDL keeps its reset value 1. CSRST and the flushes empty the FIFOs, the TX flush the FIFO
 * TXFNUM names, all of them for 0x10. CSRST clears no register.
 *
 * Link: B-valid is GOTGCTL.BVALOVAL while BVALOEN is set, else the attached host's VBUS while
 * GCCFG.VBDEN is set. BSVLD reads it, its fall sets GOTGINT.SEDET. The device pulls D+ up while
 * a host is attached, B-valid holds, GCCFG.PWRDWN powers the transceiver and DCTL.SDIS is clear,
 * and tells the host of every change. Without a host nothing attaches.
 *
 * Bus: a bus reset raises USBRST, then ENUMDNE 10 us later with DSTS.ENUMSPD 3, full speed. The
 * device answers DCFG.DAD, except after a SET_ADDRESS SETUP: it keeps the old address until the
 * status IN is acknowledged, USB 2.0 9.4.6. SOF sets GINTSTS.SOF and DSTS.FNSOF,
 * EOPF follows at the DCFG.PFIVL share of the frame. Suspend raises ESUSP, USBSUSP and
 * DSTS.SUSPSTS, resume WKUPINT.
 *
 * Receive FIFO, GRXFSIZ words: a SETUP pushes PKTSTS 6 with its 8 bytes then PKTSTS 4, takes
 * STALL off EP0, NAKs EP0 both ways, decrements STUPCNT and sets both EP0 toggles to DATA1. An OUT
 * packet on an enabled endpoint that is not NAKed is pushed as PKTSTS 2, then PKTSTS 3 when the
 * transfer completes, on PKTCNT reaching 0 or a short packet: the endpoint then disables and
 * NAKs. A packet that does not fit is NAKed. Reading GRXSTSP pops the status, reading any DFIFO
 * window pops the data words. Popping PKTSTS 4 sets DOEPINT.STUP, PKTSTS 3 DOEPINT.XFRC. Debugger
 * reads pop nothing.
 *
 * Transmit FIFOs: DFIFOn pushes to the FIFO of IN endpoint n, sized by DIEPTXF0 or DIEPTXFn.
 * DTXFSTS shows the free words, DIEPINT.TXFE the empty level GAHBCFG.TXFELVL selects. Once the
 * FIFO holds the words of a packet, the smaller of the maximum packet size and XFRSIZ, the packet
 * is recorded and XFRSIZ decrements. An IN token on an enabled endpoint that is not NAKed sends
 * the oldest recorded packet, or a zero-length one with an empty FIFO and XFRSIZ 0, else NAKs.
 * PKTCNT decrements per packet sent, XFRC and the end of the transfer follow it reaching 0.
 *
 * Host: wake-ups the host asks for are model events. The host's bytes are saved in the model
 * state, and after a load the host hears of a pull-up that differs from the saved one. A board
 * host, built from the board's usb_hosts, is owned by the model and reached through
 * usb_host_port: its cable is model state, plugged at power-on as the board says, kept across
 * other resets. Its bytes are saved with the cable unplugged too, a detached host then, so state
 * its drivers keep for a later plug survives a restore. A host given to attach() replaces it until
 * the next plug(), and is not saved.
 * Unplugging a device that pulls D+ up leaves the bus idle, as B-valid stays forced: 3 ms later
 * the device suspends as suspend() does, ESUSP, USBSUSP and DSTS.SUSPSTS. Plugging back cancels
 * a suspend still pending, and the host's bus reset ends one already taken.
 *
 * Isochronous endpoints, EPTYP 1 on EP1-5: SEVNFRM and SODDFRM pick the frame parity the
 * transfer runs in, EONUM reads it back, compared with the frame of the last SOF. Data are
 * always DATA0 and no toggle flips, NAK and STALL do not apply. An iso IN in its frame sends
 * the oldest recorded packet, a zero-length packet that completes a transfer armed with XFRSIZ
 * 0, or a zero-length packet that does not count when nothing is ready.
 * An iso OUT in its frame is taken as a bulk OUT is, a packet that does not fit is dropped. In
 * another frame, or disabled, the endpoint does not answer. At EOPF an iso endpoint enabled for
 * the frame that moved no packet raises IISOIXFR for IN, INCOMPISOOUT, GINTSTS.IPXFR, for OUT.
 * DIEPTSIZn.MCNT is ignored: one packet per frame, as full speed allows, and an iso endpoint
 * armed with PKTCNT above 1 is unmodelled. A bus reset clears USBAEP on EP1-5. Activating an IN
 * endpoint, or resizing a FIFO while one is active, warns once when GRXFSIZ, DIEPTXF0 and the
 * TX FIFOs of the active IN endpoints add up to more than the 320 words of FIFO RAM.
 *
 * Endpoint control: CNAK and SNAK set NAKSTS, SNAK also DIEPINT.INEPNE, at once. EPDIS on an
 * enabled endpoint disables it at once with EPDISD. EPENA, and STALL on EP0, are set-only: the
 * core clears them. SGINAK and SGONAK take effect at once, GINAKEFF and GONAKEFF follow DCTL
 * GINSTS and GONSTS, and SGONAK also pushes a PKTSTS 1 entry. SD0PID and SD1PID set the
 * endpoint's data toggle.
 *
 * Interrupts: GINTSTS event bits are cleared by writing 1, RXFLVL, NPTXFE, GINAKEFF, GONAKEFF,
 * IEPINT, OEPINT and OTGINT follow their sources. DAINT is DIEPINTn masked by DIEPMSK, with TXFE
 * masked by DIEPEMPMSK, and DOEPINTn masked by DOEPMSK. IRQ 73 is GAHBCFG.GINTMSK and GINTSTS
 * masked by GINTMSK.
 *
 * Not modelled, logged once and a model_error under strict_peripherals: host mode, HNP and SRP,
 * battery charging detection, test modes, a TX FIFO numbered other than its endpoint. DMA and LPM
 * are not modelled either.
 */
class otg_fs final : public dispatch_model<otg_fs, otg_fs_state>,
                     public usb::device_port,
                     public usb_host_port,
                     public bus_reset_listener
{
public:
  otg_fs(model_context& ctx, peripheral_desc const& desc, std::optional<usb_host_options> wired);

  void reset(register_file& regs, reset_kind kind) override;
  void on_bus_reset() override;
  void save(std::vector<std::byte>& out) const override;
  void load(std::span<std::byte const> in) override;
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

  void               attach(std::shared_ptr<usb::host_side> host) override;
  [[nodiscard]] auto pulled_up() const -> bool override;
  void               bus_reset(vtime at) override;
  void               start_of_frame(std::uint16_t frame, vtime at) override;
  auto setup(std::uint8_t address, usb::setup_packet packet, vtime at) -> usb::handshake override;
  auto
       out(std::uint8_t                  address,
           std::uint8_t                  endpoint,
           std::span<std::uint8_t const> data,
           usb::data_pid                 pid,
           vtime                         at) -> usb::handshake override;
  auto in(std::uint8_t address, std::uint8_t endpoint, vtime at) -> usb::in_result override;
  void suspend(vtime at) override;
  void resume(vtime at) override;
  void wake_at(vtime at) override;
  [[nodiscard]] auto quiet_until(quiet_query const& query) const -> std::optional<vtime> override;
  [[nodiscard]] auto event_pends(std::uint32_t kind, std::uint64_t payload) const -> bool;
  [[nodiscard]] auto now() const -> vtime override;

  [[nodiscard]] auto board_host() -> usb::host* override;
  [[nodiscard]] auto plugged() const -> bool override;
  void               plug(bool attached) override;

  void plain_written(std::uint32_t old, std::uint32_t now);
  void refresh_written(std::uint32_t old, std::uint32_t now);
  void fifo_size_written(std::uint32_t old, std::uint32_t now);
  void link_written(std::uint32_t old, std::uint32_t now);
  void usb_config_written(std::uint32_t old, std::uint32_t now);
  void reset_control_written(std::uint32_t old, std::uint32_t now);
  void status_written(std::uint32_t old, std::uint32_t now);
  void otg_status_written(std::uint32_t old, std::uint32_t now);
  void device_control_written(std::uint32_t old, std::uint32_t now);
  void in_control_written(std::uint32_t old, std::uint32_t now);
  void in_interrupt_written(std::uint32_t old, std::uint32_t now);
  void out_control_written(std::uint32_t old, std::uint32_t now);
  void out_interrupt_written(std::uint32_t old, std::uint32_t now);
  void fifo_written(std::uint32_t old, std::uint32_t now);

private:
  void               restore_reset_values(std::optional<reset_kind> kind);
  [[nodiscard]] auto in_fields_of(std::size_t n) const -> in_fields;
  [[nodiscard]] auto out_fields_of(std::size_t n) const -> out_fields;
  [[nodiscard]] auto b_valid() const -> bool;
  [[nodiscard]] auto on_bus(std::uint8_t address) const -> bool;
  [[nodiscard]] auto rx_used() const -> std::size_t;
  [[nodiscard]] auto tx_depth(std::size_t n) const -> std::size_t;
  [[nodiscard]] auto in_packet_size(std::size_t n) const -> std::uint32_t;
  [[nodiscard]] auto out_packet_size(std::size_t n) const -> std::uint32_t;
  [[nodiscard]] auto bus_address() const -> std::uint8_t;
  [[nodiscard]] auto frame_parity() const -> std::uint8_t;
  void               check_fifo_ram();
  template <typename Fields>
  void check_iso_packets(std::string_view reg, Fields const& ep, std::size_t n);
  void end_periodic_frame();
  auto in_isochronous(std::size_t n) -> usb::in_result;
  auto out_isochronous(std::size_t n, std::span<std::uint8_t const> data) -> usb::handshake;
  [[nodiscard]] auto unpacketed_words(std::size_t n) const -> std::size_t;
  void               packetize(std::size_t n);
  [[nodiscard]] auto is_control(std::uint32_t word) const -> bool;
  [[nodiscard]] auto is_fifo(std::uint32_t word) const -> bool;
  void               link(bool report_session_end);
  void               refresh();
  void               flush_rx();
  void               flush_tx(std::uint32_t number);
  void               push_rx(std::uint32_t status, std::vector<std::uint32_t> words);
  auto               pop_status() -> std::uint32_t;
  auto               pop_data() -> std::uint32_t;
  void               schedule(std::uint32_t kind, std::uint64_t at_ps);
  void               unmodelled(std::string_view name, std::string message, warning kind);
  void               notice(std::string_view name, std::string message, warning kind);
  void               select_host();
  void               enter_suspend();

  register_file&                    regs_;
  std::shared_ptr<usb::host_side>   host_;
  std::shared_ptr<usb::host>        board_host_;
  bool                              plugged_at_power_on_{};
  irq_line&                         line_;
  std::array<in_fields, endpoints>  in_;
  std::array<out_fields, endpoints> out_;
  reg_ref                           rx_status_peek_;
  reg_ref                           rx_status_pop_;
  reg_ref                           fifo_window_;
  reg_ref                           otg_interrupt_;
  reg_ref                           core_status_;
  reg_ref                           core_mask_;
  reg_ref                           in_mask_;
  reg_ref                           out_mask_;
  reg_ref                           all_endpoints_;
  reg_ref                           all_endpoints_mask_;
  field_ref                         b_valid_override_;
  field_ref                         b_valid_value_;
  field_ref                         b_session_valid_;
  field_ref                         session_request_;
  field_ref                         host_negotiation_;
  field_ref                         session_end_;
  field_ref                         global_interrupt_;
  field_ref                         tx_empty_level_;
  field_ref                         force_host_;
  std::array<field_ref, 5>          self_clearing_;
  field_ref                         core_reset_;
  field_ref                         rx_flush_;
  field_ref                         tx_flush_;
  field_ref                         tx_flush_number_;
  std::uint32_t                     clear_on_one_{};
  field_ref                         bus_reset_;
  field_ref                         enumeration_done_;
  field_ref                         start_of_frame_;
  field_ref                         end_of_periodic_;
  field_ref                         early_suspend_;
  field_ref                         suspend_;
  field_ref                         wake_up_;
  field_ref                         incomplete_iso_in_;
  field_ref                         incomplete_iso_out_;
  field_ref                         otg_pending_;
  field_ref                         rx_level_;
  field_ref                         np_tx_empty_;
  field_ref                         in_nak_effective_;
  field_ref                         out_nak_effective_;
  field_ref                         in_pending_;
  field_ref                         out_pending_;
  field_ref                         rx_depth_;
  field_ref                         power_up_;
  field_ref                         vbus_detection_;
  field_ref                         charging_detection_;
  field_ref                         address_;
  field_ref                         periodic_interval_;
  field_ref                         soft_disconnect_;
  field_ref                         in_nak_status_;
  field_ref                         out_nak_status_;
  field_ref                         set_in_nak_;
  field_ref                         clear_in_nak_;
  field_ref                         set_out_nak_;
  field_ref                         clear_out_nak_;
  field_ref                         test_control_;
  field_ref                         suspended_status_;
  field_ref                         speed_;
  field_ref                         frame_number_;
  field_ref                         empty_mask_;
  std::uint32_t                     access_offset_{};
  std::uint32_t                     written_{};
  std::uint32_t                     warned_{};
};

constexpr std::array<write_entry<otg_fs>, 73> otg_fs_writes{
    {
     {"GOTGCTL", &otg_fs::link_written},
     {"GOTGINT", &otg_fs::otg_status_written},
     {"GAHBCFG", &otg_fs::refresh_written},
     {"GUSBCFG", &otg_fs::usb_config_written},
     {"GRSTCTL", &otg_fs::reset_control_written},
     {"GINTSTS", &otg_fs::status_written},
     {"GINTMSK", &otg_fs::refresh_written},
     {"GRXSTSR_DEVICE", &otg_fs::plain_written},
     {"GRXSTSP_DEVICE", &otg_fs::plain_written},
     {"GRXFSIZ", &otg_fs::fifo_size_written},
     {"HNPTXFSIZ", &otg_fs::fifo_size_written},
     {"GCCFG", &otg_fs::link_written},
     {"DIEPTXF1", &otg_fs::fifo_size_written},
     {"DIEPTXF2", &otg_fs::fifo_size_written},
     {"DIEPTXF3", &otg_fs::fifo_size_written},
     {"DIEPTXF4", &otg_fs::fifo_size_written},
     {"DIEPTXF5", &otg_fs::fifo_size_written},
     {"DCFG", &otg_fs::plain_written},
     {"DCTL", &otg_fs::device_control_written},
     {"DSTS", &otg_fs::plain_written},
     {"DIEPMSK", &otg_fs::refresh_written},
     {"DOEPMSK", &otg_fs::refresh_written},
     {"DAINT", &otg_fs::plain_written},
     {"DAINTMSK", &otg_fs::refresh_written},
     {"DIEPEMPMSK", &otg_fs::refresh_written},
     {"DIEPCTL0", &otg_fs::in_control_written},
     {"DIEPCTL1", &otg_fs::in_control_written},
     {"DIEPCTL2", &otg_fs::in_control_written},
     {"DIEPCTL3", &otg_fs::in_control_written},
     {"DIEPCTL4", &otg_fs::in_control_written},
     {"DIEPCTL5", &otg_fs::in_control_written},
     {"DIEPINT0", &otg_fs::in_interrupt_written},
     {"DIEPINT1", &otg_fs::in_interrupt_written},
     {"DIEPINT2", &otg_fs::in_interrupt_written},
     {"DIEPINT3", &otg_fs::in_interrupt_written},
     {"DIEPINT4", &otg_fs::in_interrupt_written},
     {"DIEPINT5", &otg_fs::in_interrupt_written},
     {"DIEPTSIZ0", &otg_fs::plain_written},
     {"DIEPTSIZ1", &otg_fs::plain_written},
     {"DIEPTSIZ2", &otg_fs::plain_written},
     {"DIEPTSIZ3", &otg_fs::plain_written},
     {"DIEPTSIZ4", &otg_fs::plain_written},
     {"DIEPTSIZ5", &otg_fs::plain_written},
     {"DTXFSTS0", &otg_fs::plain_written},
     {"DTXFSTS1", &otg_fs::plain_written},
     {"DTXFSTS2", &otg_fs::plain_written},
     {"DTXFSTS3", &otg_fs::plain_written},
     {"DTXFSTS4", &otg_fs::plain_written},
     {"DTXFSTS5", &otg_fs::plain_written},
     {"DOEPCTL0", &otg_fs::out_control_written},
     {"DOEPCTL1", &otg_fs::out_control_written},
     {"DOEPCTL2", &otg_fs::out_control_written},
     {"DOEPCTL3", &otg_fs::out_control_written},
     {"DOEPCTL4", &otg_fs::out_control_written},
     {"DOEPCTL5", &otg_fs::out_control_written},
     {"DOEPINT0", &otg_fs::out_interrupt_written},
     {"DOEPINT1", &otg_fs::out_interrupt_written},
     {"DOEPINT2", &otg_fs::out_interrupt_written},
     {"DOEPINT3", &otg_fs::out_interrupt_written},
     {"DOEPINT4", &otg_fs::out_interrupt_written},
     {"DOEPINT5", &otg_fs::out_interrupt_written},
     {"DOEPTSIZ0", &otg_fs::plain_written},
     {"DOEPTSIZ1", &otg_fs::plain_written},
     {"DOEPTSIZ2", &otg_fs::plain_written},
     {"DOEPTSIZ3", &otg_fs::plain_written},
     {"DOEPTSIZ4", &otg_fs::plain_written},
     {"DOEPTSIZ5", &otg_fs::plain_written},
     {"DFIFO0", &otg_fs::fifo_written},
     {"DFIFO1", &otg_fs::fifo_written},
     {"DFIFO2", &otg_fs::fifo_written},
     {"DFIFO3", &otg_fs::fifo_written},
     {"DFIFO4", &otg_fs::fifo_written},
     {"DFIFO5", &otg_fs::fifo_written},
     }
};

otg_fs::otg_fs(
    model_context&                  ctx,
    peripheral_desc const&          desc,
    std::optional<usb_host_options> wired)
    : dispatch_model{ctx, desc, "OTG_FS", otg_fs_writes}, regs_{ctx.regs},
      line_{irq("OTG_FS_IRQn")}, in_{[this]<std::size_t... n>(std::index_sequence<n...>) {
        return std::array<in_fields, endpoints>{in_fields_of(n)...};
      }(std::make_index_sequence<endpoints>{})},
      out_{[this]<std::size_t... n>(std::index_sequence<n...>) {
        return std::array<out_fields, endpoints>{out_fields_of(n)...};
      }(std::make_index_sequence<endpoints>{})},
      rx_status_peek_{reg("GRXSTSR_DEVICE")}, rx_status_pop_{reg("GRXSTSP_DEVICE")},
      fifo_window_{reg("DFIFO0")}, otg_interrupt_{reg("GOTGINT")}, core_status_{reg("GINTSTS")},
      core_mask_{reg("GINTMSK")}, in_mask_{reg("DIEPMSK")}, out_mask_{reg("DOEPMSK")},
      all_endpoints_{reg("DAINT")}, all_endpoints_mask_{reg("DAINTMSK")},
      b_valid_override_{field("GOTGCTL", "BVALOEN")}, b_valid_value_{field("GOTGCTL", "BVALOVAL")},
      b_session_valid_{field("GOTGCTL", "BSVLD")}, session_request_{field("GOTGCTL", "SRQ")},
      host_negotiation_{field("GOTGCTL", "HNPRQ")}, session_end_{field("GOTGINT", "SEDET")},
      global_interrupt_{field("GAHBCFG", "GINTMSK")}, tx_empty_level_{field("GAHBCFG", "TXFELVL")},
      force_host_{field("GUSBCFG", "FHMOD")},
      self_clearing_{
          field("GRSTCTL", "CSRST"), field("GRSTCTL", "PSRST"), field("GRSTCTL", "FSRST"),
          field("GRSTCTL", "RXFFLSH"), field("GRSTCTL", "TXFFLSH")},
      core_reset_{field("GRSTCTL", "CSRST")}, rx_flush_{field("GRSTCTL", "RXFFLSH")},
      tx_flush_{field("GRSTCTL", "TXFFLSH")}, tx_flush_number_{field("GRSTCTL", "TXFNUM")},
      bus_reset_{field("GINTSTS", "USBRST")}, enumeration_done_{field("GINTSTS", "ENUMDNE")},
      start_of_frame_{field("GINTSTS", "SOF")}, end_of_periodic_{field("GINTSTS", "EOPF")},
      early_suspend_{field("GINTSTS", "ESUSP")}, suspend_{field("GINTSTS", "USBSUSP")},
      wake_up_{field("GINTSTS", "WKUPINT")}, incomplete_iso_in_{field("GINTSTS", "IISOIXFR")},
      incomplete_iso_out_{field("GINTSTS", "IPXFR")}, otg_pending_{field("GINTSTS", "OTGINT")},
      rx_level_{field("GINTSTS", "RXFLVL")}, np_tx_empty_{field("GINTSTS", "NPTXFE")},
      in_nak_effective_{field("GINTSTS", "GINAKEFF")},
      out_nak_effective_{field("GINTSTS", "GONAKEFF")}, in_pending_{field("GINTSTS", "IEPINT")},
      out_pending_{field("GINTSTS", "OEPINT")}, rx_depth_{field("GRXFSIZ", "RXFD")},
      power_up_{field("GCCFG", "PWRDWN")}, vbus_detection_{field("GCCFG", "VBDEN")},
      charging_detection_{field("GCCFG", "BCDEN")}, address_{field("DCFG", "DAD")},
      periodic_interval_{field("DCFG", "PFIVL")}, soft_disconnect_{field("DCTL", "SDIS")},
      in_nak_status_{field("DCTL", "GINSTS")}, out_nak_status_{field("DCTL", "GONSTS")},
      set_in_nak_{field("DCTL", "SGINAK")}, clear_in_nak_{field("DCTL", "CGINAK")},
      set_out_nak_{field("DCTL", "SGONAK")}, clear_out_nak_{field("DCTL", "CGONAK")},
      test_control_{field("DCTL", "TCTL")}, suspended_status_{field("DSTS", "SUSPSTS")},
      speed_{field("DSTS", "ENUMSPD")}, frame_number_{field("DSTS", "FNSOF")},
      empty_mask_{field("DIEPEMPMSK", "INEPTXFEM")}
{
  for(auto const* name :
      {"MMIS", "SOF", "ESUSP", "USBSUSP", "USBRST", "ENUMDNE", "ISOODRP", "EOPF", "IISOIXFR",
       "IPXFR", "RSTDET", "LPMINT", "CIDSCHG", "DISCINT", "SRQINT", "WKUPINT"})
  {
    clear_on_one_ |= field("GINTSTS", name).mask();
  }
  if(wired)
  {
    if(not wired->host.warn)
    {
      wired->host.warn = [this](std::string const& message) { warn("host", message); };
    }
    board_host_          = std::make_shared<usb::host>(*this, std::move(wired->host));
    plugged_at_power_on_ = wired->attached;
    state_.cable         = wired->attached;
    select_host();
  }
}

void otg_fs::select_host()
{
  host_ = state_.cable ? board_host_ : nullptr;
}

auto otg_fs::in_fields_of(std::size_t n) const -> in_fields
{
  auto const control  = std::format("DIEPCTL{}", n);
  auto const status   = std::format("DIEPINT{}", n);
  auto const transfer = std::format("DIEPTSIZ{}", n);
  return {
      .type          = field(control, "EPTYP"),
      .packet_size   = field(control, "MPSIZ"),
      .active        = field(control, "USBAEP"),
      .nak           = field(control, "NAKSTS"),
      .stall         = field(control, "STALL"),
      .fifo_number   = field(control, "TXFNUM"),
      .clear_nak     = field(control, "CNAK"),
      .set_nak       = field(control, "SNAK"),
      .set_data0     = n == 0 ? std::nullopt : std::optional{field(control, "SD0PID_SEVNFRM")},
      .set_data1     = n == 0 ? std::nullopt : std::optional{field(control, "SODDFRM")},
      .data_pid      = n == 0 ? std::nullopt : std::optional{field(control, "EONUM_DPIP")},
      .disable       = field(control, "EPDIS"),
      .enable        = field(control, "EPENA"),
      .complete      = field(status, "XFRC"),
      .disabled      = field(status, "EPDISD"),
      .nak_effective = field(status, "INEPNE"),
      .fifo_empty    = field(status, "TXFE"),
      .size          = field(transfer, "XFRSIZ"),
      .packets       = field(transfer, "PKTCNT"),
      .available     = field(std::format("DTXFSTS{}", n), "INEPTFSAV"),
      .depth =
          n == 0 ? field("HNPTXFSIZ", "NPTXFD") : field(std::format("DIEPTXF{}", n), "INEPTXFD"),
  };
}

auto otg_fs::out_fields_of(std::size_t n) const -> out_fields
{
  auto const control  = std::format("DOEPCTL{}", n);
  auto const status   = std::format("DOEPINT{}", n);
  auto const transfer = std::format("DOEPTSIZ{}", n);
  return {
      .type        = field(control, "EPTYP"),
      .active      = field(control, "USBAEP"),
      .packet_size = field(control, "MPSIZ"),
      .nak         = field(control, "NAKSTS"),
      .stall       = field(control, "STALL"),
      .clear_nak   = field(control, "CNAK"),
      .set_nak     = field(control, "SNAK"),
      .set_data0   = n == 0 ? std::nullopt : std::optional{field(control, "SD0PID_SEVNFRM")},
      .set_data1   = n == 0 ? std::nullopt : std::optional{field(control, "SD1PID_SODDFRM")},
      .data_pid    = n == 0 ? std::nullopt : std::optional{field(control, "EONUM_DPIP")},
      .disable     = field(control, "EPDIS"),
      .enable      = field(control, "EPENA"),
      .complete    = field(status, "XFRC"),
      .disabled    = field(status, "EPDISD"),
      .setup_done  = field(status, "STUP"),
      .size        = field(transfer, "XFRSIZ"),
      .packets     = field(transfer, "PKTCNT"),
      .setup_count = n == 0 ? std::optional{field(transfer, "STUPCNT")} : std::nullopt,
  };
}

void otg_fs::reset(register_file&, reset_kind kind)
{
  restore_reset_values(kind);
}

void otg_fs::on_bus_reset()
{
  restore_reset_values(std::nullopt);
}

void otg_fs::restore_reset_values(std::optional<reset_kind> kind)
{
  for(auto const& name : claims())
  {
    regs_.poke(reg(name).offset, describe(name).reset);
  }
  auto const generation = state_.generation;
  auto const pulled     = state_.pulled_up;
  auto const cable      = state_.cable;
  state_                = {};
  state_.generation     = generation;
  for(auto& count : state_.generation)
  {
    ++count;
  }
  state_.pulled_up = pulled;
  state_.cable     = cable;
  if(board_host_ and kind == reset_kind::power_on and cable != plugged_at_power_on_)
  {
    plug(plugged_at_power_on_);
  }
  link(false);
  refresh();
}

auto otg_fs::now() const -> vtime
{
  return ctx_.clock.time();
}

auto otg_fs::b_valid() const -> bool
{
  if(get(regs_, b_valid_override_) != 0)
  {
    return get(regs_, b_valid_value_) != 0;
  }
  return host_ != nullptr and get(regs_, vbus_detection_) != 0;
}

auto otg_fs::pulled_up() const -> bool
{
  return host_ != nullptr
     and b_valid()
     and get(regs_, power_up_) != 0
     and get(regs_, soft_disconnect_) == 0;
}

auto otg_fs::on_bus(std::uint8_t address) const -> bool
{
  return pulled_up() and address == bus_address();
}

auto otg_fs::bus_address() const -> std::uint8_t
{
  return state_.address_pending ? state_.address : static_cast<std::uint8_t>(get(regs_, address_));
}

auto otg_fs::rx_used() const -> std::size_t
{
  auto used = state_.rx_data.size();
  for(auto const& packet : state_.rx_fifo)
  {
    used += 1 + packet.words.size();
  }
  return used;
}

auto otg_fs::tx_depth(std::size_t n) const -> std::size_t
{
  return get(regs_, in_[n].depth);
}

auto otg_fs::in_packet_size(std::size_t n) const -> std::uint32_t
{
  if(n == 0)
  {
    return ep0_packet[get(regs_, in_[0].packet_size) & 3u];
  }
  return get(regs_, in_[n].packet_size);
}

auto otg_fs::out_packet_size(std::size_t n) const -> std::uint32_t
{
  if(n == 0)
  {
    return in_packet_size(0);
  }
  return get(regs_, out_[n].packet_size);
}

auto otg_fs::unpacketed_words(std::size_t n) const -> std::size_t
{
  auto packeted = std::size_t{0};
  for(auto const bytes : state_.tx_packets[n])
  {
    packeted += words_for(bytes);
  }
  return state_.tx_fifo[n].size() - packeted;
}

void otg_fs::packetize(std::size_t n)
{
  auto const& ep        = in_[n];
  auto const  remaining = get(regs_, ep.size);
  auto const  bytes     = std::min(in_packet_size(n), remaining);
  if(bytes > 0 and unpacketed_words(n) >= words_for(bytes))
  {
    state_.tx_packets[n].push_back(static_cast<std::uint16_t>(bytes));
    update(regs_, ep.size, remaining - bytes);
  }
}

auto otg_fs::is_control(std::uint32_t word) const -> bool
{
  for(auto const base : {in_[0].enable.offset, out_[0].enable.offset})
  {
    if(word >= base
       and word < base + endpoints * endpoint_stride
       and (word - base) % endpoint_stride == 0)
    {
      return true;
    }
  }
  return false;
}

auto otg_fs::is_fifo(std::uint32_t word) const -> bool
{
  auto const base = fifo_window_.offset;
  return word >= base
     and word < base + endpoints * window_stride
     and (word - base) % window_stride == 0;
}

void otg_fs::link(bool report_session_end)
{
  auto const valid = b_valid();
  update(regs_, b_session_valid_, valid ? 1 : 0);
  if(report_session_end and state_.b_valid and not valid)
  {
    update(regs_, session_end_, 1);
  }
  state_.b_valid = valid;
  auto const up  = pulled_up();
  if(up != state_.pulled_up)
  {
    state_.pulled_up = up;
    if(host_)
    {
      host_->pull_up_changed(up, now());
    }
  }
}

void otg_fs::refresh()
{
  auto const    whole_empty = get(regs_, tx_empty_level_) != 0;
  auto const    in_mask     = regs_.peek(in_mask_.offset);
  auto const    out_mask    = regs_.peek(out_mask_.offset);
  auto const    empty_mask  = get(regs_, empty_mask_);
  std::uint32_t pending     = 0;
  for(std::size_t n = 0; n < endpoints; ++n)
  {
    auto const& in    = in_[n];
    auto const  used  = state_.tx_fifo[n].size();
    auto const  depth = tx_depth(n);
    update(regs_, in.available, static_cast<std::uint32_t>(depth > used ? depth - used : 0));
    update(regs_, in.fifo_empty, (whole_empty ? used == 0 : 2 * used <= depth) ? 1 : 0);
    if(in.data_pid)
    {
      update(regs_, *in.data_pid, state_.in[n].toggle);
    }
    auto const shown = in_mask | (((empty_mask >> n) & 1u) != 0 ? in.fifo_empty.mask() : 0u);
    if((regs_.peek(in.complete.offset) & shown) != 0)
    {
      pending |= 1u << n;
    }
    auto const& out = out_[n];
    if(out.data_pid)
    {
      update(regs_, *out.data_pid, state_.out[n].toggle);
    }
    if((regs_.peek(out.complete.offset) & out_mask) != 0)
    {
      pending |= 1u << (16 + n);
    }
  }
  regs_.poke(all_endpoints_.offset, pending);
  auto const unmasked = pending & regs_.peek(all_endpoints_mask_.offset);
  update(regs_, in_pending_, (unmasked & 0xffffu) != 0 ? 1 : 0);
  update(regs_, out_pending_, (unmasked >> 16) != 0 ? 1 : 0);
  update(regs_, np_tx_empty_, get(regs_, in_[0].fifo_empty));
  update(regs_, rx_level_, state_.rx_fifo.empty() ? 0 : 1);
  update(regs_, otg_pending_, regs_.peek(otg_interrupt_.offset) != 0 ? 1 : 0);
  update(regs_, in_nak_effective_, get(regs_, in_nak_status_));
  update(regs_, out_nak_effective_, get(regs_, out_nak_status_));
  auto const head = state_.rx_fifo.empty() ? 0u : state_.rx_fifo.front().status;
  regs_.poke(rx_status_peek_.offset, head);
  regs_.poke(rx_status_pop_.offset, head);
  auto const raised = get(regs_, global_interrupt_) != 0
                  and (regs_.peek(core_status_.offset) & regs_.peek(core_mask_.offset)) != 0;
  if(raised)
  {
    line_.raise();
  }
  else
  {
    line_.lower();
  }
}

void otg_fs::flush_rx()
{
  state_.rx_fifo.clear();
  state_.rx_data.clear();
}

void otg_fs::flush_tx(std::uint32_t number)
{
  if(number == all_tx_fifos)
  {
    for(std::size_t n = 0; n < endpoints; ++n)
    {
      state_.tx_fifo[n].clear();
      state_.tx_packets[n].clear();
    }
  }
  else if(number < endpoints)
  {
    state_.tx_fifo[number].clear();
    state_.tx_packets[number].clear();
  }
}

void otg_fs::push_rx(std::uint32_t status, std::vector<std::uint32_t> words)
{
  state_.rx_fifo.push_back({.status = status, .words = std::move(words)});
}

auto otg_fs::pop_status() -> std::uint32_t
{
  if(state_.rx_fifo.empty())
  {
    return 0;
  }
  if(not state_.rx_data.empty())
  {
    notice(
        "GRXSTSP_DEVICE",
        "GRXSTSP popped before the data of the previous packet were read: they are dropped",
        rx_unread);
  }
  auto packet = std::move(state_.rx_fifo.front());
  state_.rx_fifo.erase(state_.rx_fifo.begin());
  state_.rx_data      = std::move(packet.words);
  auto const endpoint = packet.status & 0xfu;
  auto const pktsts   = pktsts_of(packet.status);
  if(endpoint < endpoints and pktsts == pktsts_out_done)
  {
    update(regs_, out_[endpoint].complete, 1);
  }
  if(endpoint < endpoints and pktsts == pktsts_setup_done)
  {
    update(regs_, out_[endpoint].setup_done, 1);
  }
  refresh();
  return packet.status;
}

auto otg_fs::pop_data() -> std::uint32_t
{
  if(state_.rx_data.empty())
  {
    notice("DFIFO0", "read of the receive FIFO with no data popped: reads 0", rx_empty);
    return 0;
  }
  auto const word = state_.rx_data.front();
  state_.rx_data.erase(state_.rx_data.begin());
  return word;
}

void otg_fs::schedule(std::uint32_t kind, std::uint64_t at_ps)
{
  ctx_.events.schedule_at(
      vtime{std::max(at_ps, now().ps)}, kind, ++state_.generation[kind], ctx_.clock);
}

void otg_fs::notice(std::string_view name, std::string message, warning kind)
{
  if((warned_ & kind) == 0)
  {
    warned_ |= kind;
    warn(name, std::move(message));
  }
}

void otg_fs::unmodelled(std::string_view name, std::string message, warning kind)
{
  notice(name, std::move(message), kind);
  if(ctx_.strict_peripherals)
  {
    throw model_error(desc_.name, std::string{name}, reg(name).offset);
  }
}

auto otg_fs::on_read(register_file& regs, std::uint32_t offset, std::uint8_t size, access_ctx ctx)
    -> std::expected<std::uint32_t, bus_error>
{
  auto const word = offset & ~3u;
  if(ctx.by != initiator::debugger and word == rx_status_pop_.offset)
  {
    return read_lanes(pop_status(), offset, size);
  }
  if(ctx.by != initiator::debugger and is_fifo(word))
  {
    return read_lanes(pop_data(), offset, size);
  }
  if(is_control(word))
  {
    return read_lanes(regs.peek(word), offset, size);
  }
  return dispatch_model::on_read(regs, offset, size, ctx);
}

auto otg_fs::on_write(
    register_file& regs,
    std::uint32_t  offset,
    std::uint8_t   size,
    std::uint32_t  old,
    std::uint32_t  value,
    access_ctx     ctx) -> std::expected<void, bus_error>
{
  access_offset_ = offset & ~3u;
  written_       = merge_write(0, offset, size, value);
  return dispatch_model::on_write(regs, offset, size, old, value, ctx);
}

void otg_fs::plain_written(std::uint32_t, std::uint32_t)
{}

void otg_fs::refresh_written(std::uint32_t, std::uint32_t)
{
  refresh();
}

void otg_fs::fifo_size_written(std::uint32_t, std::uint32_t)
{
  check_fifo_ram();
  refresh();
}

void otg_fs::link_written(std::uint32_t, std::uint32_t)
{
  if(get(regs_, session_request_) != 0 or get(regs_, host_negotiation_) != 0)
  {
    unmodelled(
        "GOTGCTL", "session request and host negotiation, GOTGCTL SRQ and HNPRQ, are not modelled",
        otg_negotiation);
  }
  if(get(regs_, charging_detection_) != 0)
  {
    unmodelled(
        "GCCFG", "battery charging detection, GCCFG.BCDEN, is not modelled", battery_charging);
  }
  link(true);
  refresh();
}

void otg_fs::usb_config_written(std::uint32_t, std::uint32_t now)
{
  if(force_host_.of(now) != 0)
  {
    unmodelled(
        "GUSBCFG", "host mode, GUSBCFG.FHMOD, is not modelled: the core stays a device", host_mode);
  }
}

void otg_fs::reset_control_written(std::uint32_t, std::uint32_t now)
{
  if(core_reset_.of(now) != 0 or rx_flush_.of(now) != 0)
  {
    flush_rx();
  }
  if(core_reset_.of(now) != 0)
  {
    flush_tx(all_tx_fifos);
  }
  if(tx_flush_.of(now) != 0)
  {
    flush_tx(tx_flush_number_.of(now));
  }
  for(auto const bit : self_clearing_)
  {
    update(regs_, bit, 0);
  }
  refresh();
}

void otg_fs::status_written(std::uint32_t old, std::uint32_t)
{
  regs_.poke(core_status_.offset, old & ~(written_ & clear_on_one_));
  refresh();
}

void otg_fs::otg_status_written(std::uint32_t old, std::uint32_t)
{
  regs_.poke(otg_interrupt_.offset, old & ~written_);
  refresh();
}

void otg_fs::device_control_written(std::uint32_t, std::uint32_t now)
{
  if(set_in_nak_.of(now) != 0)
  {
    update(regs_, in_nak_status_, 1);
  }
  if(clear_in_nak_.of(now) != 0)
  {
    update(regs_, in_nak_status_, 0);
  }
  if(set_out_nak_.of(now) != 0)
  {
    if(get(regs_, out_nak_status_) == 0)
    {
      push_rx(status_word(0, 0, 0, pktsts_global_out_nak, state_.frame), {});
    }
    update(regs_, out_nak_status_, 1);
  }
  if(clear_out_nak_.of(now) != 0)
  {
    update(regs_, out_nak_status_, 0);
  }
  for(auto const bit : {set_in_nak_, clear_in_nak_, set_out_nak_, clear_out_nak_})
  {
    update(regs_, bit, 0);
  }
  if(test_control_.of(now) != 0)
  {
    unmodelled("DCTL", "test modes, DCTL.TCTL, are not modelled", test_mode);
  }
  link(true);
  refresh();
}

void otg_fs::in_control_written(std::uint32_t old, std::uint32_t now)
{
  auto const  n  = (access_offset_ - in_[0].enable.offset) / endpoint_stride;
  auto const& ep = in_[n];
  update(regs_, ep.enable, ep.enable.of(old) | ep.enable.of(now));
  if(n == 0)
  {
    update(regs_, ep.stall, ep.stall.of(old) | ep.stall.of(now));
  }
  if(ep.clear_nak.of(now) != 0)
  {
    update(regs_, ep.nak, 0);
    update(regs_, ep.nak_effective, 0);
  }
  if(ep.set_nak.of(now) != 0)
  {
    update(regs_, ep.nak, 1);
    update(regs_, ep.nak_effective, 1);
  }
  if(ep.set_data0 and ep.set_data0->of(now) != 0)
  {
    state_.in[n].toggle = 0;
  }
  if(ep.set_data1 and ep.set_data1->of(now) != 0)
  {
    state_.in[n].toggle = 1;
  }
  if(ep.disable.of(now) != 0 and get(regs_, ep.enable) != 0)
  {
    update(regs_, ep.enable, 0);
    update(regs_, ep.disabled, 1);
  }
  update(regs_, ep.disable, 0);
  update(regs_, ep.clear_nak, 0);
  update(regs_, ep.set_nak, 0);
  for(auto const& bit : {ep.set_data0, ep.set_data1})
  {
    if(bit)
    {
      update(regs_, *bit, 0);
    }
  }
  if(n != 0 and get(regs_, ep.enable) != 0 and get(regs_, ep.fifo_number) != n)
  {
    unmodelled(
        std::format("DIEPCTL{}", n),
        "a TX FIFO numbered other than its IN endpoint, DIEPCTLn.TXFNUM != n, is not modelled: "
        "DFIFOn feeds IN endpoint n",
        shared_tx_fifo);
  }
  if(n != 0 and ep.active.of(old) == 0 and ep.active.of(now) != 0)
  {
    check_fifo_ram();
  }
  check_iso_packets(std::format("DIEPCTL{}", n), ep, n);
  refresh();
}

void otg_fs::in_interrupt_written(std::uint32_t old, std::uint32_t)
{
  auto const  n     = (access_offset_ - in_[0].complete.offset) / endpoint_stride;
  auto const& ep    = in_[n];
  auto const  fixed = ep.fifo_empty.mask() | ep.nak_effective.mask();
  regs_.poke(access_offset_, old & ~(written_ & ~fixed));
  refresh();
}

void otg_fs::out_control_written(std::uint32_t old, std::uint32_t now)
{
  auto const  n  = (access_offset_ - out_[0].enable.offset) / endpoint_stride;
  auto const& ep = out_[n];
  update(regs_, ep.enable, ep.enable.of(old) | ep.enable.of(now));
  if(n == 0)
  {
    update(regs_, ep.stall, ep.stall.of(old) | ep.stall.of(now));
  }
  if(ep.clear_nak.of(now) != 0)
  {
    update(regs_, ep.nak, 0);
  }
  if(ep.set_nak.of(now) != 0)
  {
    update(regs_, ep.nak, 1);
  }
  if(ep.set_data0 and ep.set_data0->of(now) != 0)
  {
    state_.out[n].toggle = 0;
  }
  if(ep.set_data1 and ep.set_data1->of(now) != 0)
  {
    state_.out[n].toggle = 1;
  }
  if(ep.disable.of(now) != 0 and get(regs_, ep.enable) != 0)
  {
    update(regs_, ep.enable, 0);
    update(regs_, ep.disabled, 1);
  }
  update(regs_, ep.disable, 0);
  update(regs_, ep.clear_nak, 0);
  update(regs_, ep.set_nak, 0);
  for(auto const& bit : {ep.set_data0, ep.set_data1})
  {
    if(bit)
    {
      update(regs_, *bit, 0);
    }
  }
  check_iso_packets(std::format("DOEPCTL{}", n), ep, n);
  refresh();
}

void otg_fs::out_interrupt_written(std::uint32_t old, std::uint32_t)
{
  regs_.poke(access_offset_, old & ~written_);
  refresh();
}

void otg_fs::fifo_written(std::uint32_t, std::uint32_t)
{
  auto const n    = (access_offset_ - fifo_window_.offset) / window_stride;
  auto&      fifo = state_.tx_fifo[n];
  if(fifo.size() < tx_depth(n))
  {
    fifo.push_back(written_);
    packetize(n);
  }
  else
  {
    notice(
        std::format("DFIFO{}", n),
        std::format("push to the full TX FIFO {}: the word is dropped", n), tx_overflow);
  }
  regs_.poke(access_offset_, 0);
  refresh();
}

void otg_fs::on_event(std::uint32_t kind, std::uint64_t payload)
{
  if(kind >= state_.generation.size() or payload != state_.generation[kind])
  {
    return;
  }
  if(kind == enumeration_done)
  {
    update(regs_, speed_, full_speed_48mhz);
    update(regs_, enumeration_done_, 1);
  }
  else if(kind == periodic_end)
  {
    end_periodic_frame();
  }
  else if(kind == idle_suspend)
  {
    if(not state_.cable)
    {
      enter_suspend();
    }
  }
  else if(host_)
  {
    host_->on_time(vtime{state_.wake_ps});
    return;
  }
  refresh();
}

auto otg_fs::quiet_until(quiet_query const& query) const -> std::optional<vtime>
{
  return query.next_event(*this, [this](std::uint32_t kind, std::uint64_t payload) {
    return event_pends(kind, payload);
  });
}

auto otg_fs::event_pends(std::uint32_t kind, std::uint64_t payload) const -> bool
{
  if(kind >= state_.generation.size() or payload != state_.generation[kind])
  {
    return false;
  }
  if(kind == enumeration_done or kind == idle_suspend)
  {
    return true;
  }
  auto const unmasked = [&](field_ref const& flag) {
    return get(regs_, global_interrupt_) != 0
       and (regs_.peek(core_mask_.offset) & flag.mask()) != 0;
  };
  if(kind == periodic_end)
  {
    return unmasked(end_of_periodic_)
        or unmasked(incomplete_iso_in_)
        or unmasked(incomplete_iso_out_);
  }
  if(not host_ or not board_host_)
  {
    return host_ != nullptr;
  }
  auto const work     = board_host_->pending();
  auto       in_ready = false;
  auto       out_open = false;
  for(std::size_t n = 0; n < endpoints; ++n)
  {
    auto const& in = in_[n];
    if(get(regs_, in.enable) != 0 and (n == 0 or get(regs_, in.active) != 0))
    {
      if(n != 0 and get(regs_, in.type) == isochronous)
      {
        in_ready = true;
      }
      else if(
          get(regs_, in.packets) != 0
          and get(regs_, in.stall) == 0
          and get(regs_, in.nak) == 0
          and get(regs_, in_nak_status_) == 0
          and (not state_.tx_packets[n].empty() or (state_.tx_fifo[n].empty() and get(regs_, in.size) == 0)))
      {
        in_ready = true;
      }
    }
    out_open = out_open or get(regs_, out_[n].enable) != 0;
  }
  return work.port
      or unmasked(start_of_frame_)
      or work.control
      or (work.out and out_open)
      or in_ready;
}

void otg_fs::wake_at(vtime at)
{
  state_.wake_ps = std::max(at.ps, now().ps);
  schedule(host_wake, state_.wake_ps);
}

void otg_fs::save(std::vector<std::byte>& out) const
{
  auto saved = state_;
  saved.host.clear();
  if(board_host_ and (host_ == board_host_ or host_ == nullptr))
  {
    board_host_->save(saved.host);
  }
  else if(not board_host_ and host_ != nullptr)
  {
    host_->save(saved.host);
  }
  reflex::serde::bson::serializer{out}.dump(saved);
}

void otg_fs::load(std::span<std::byte const> in)
{
  dispatch_model::load(in);
  auto const blob = std::exchange(state_.host, {});
  if(board_host_)
  {
    select_host();
    board_host_->load(blob);
  }
  else if(host_)
  {
    host_->load(blob);
  }
  link(false);
}

void otg_fs::attach(std::shared_ptr<usb::host_side> host)
{
  host_ = std::move(host);
  link(true);
  refresh();
}

auto otg_fs::board_host() -> usb::host*
{
  return board_host_.get();
}

auto otg_fs::plugged() const -> bool
{
  return board_host_ != nullptr and state_.cable;
}

void otg_fs::plug(bool attached)
{
  if(not board_host_)
  {
    return;
  }
  auto const was_up = state_.pulled_up and host_ == board_host_;
  state_.cable      = attached;
  select_host();
  link(true);
  if(attached)
  {
    ++state_.generation[idle_suspend];
  }
  else if(was_up)
  {
    board_host_->pull_up_changed(false, now());
    schedule(idle_suspend, now().ps + idle_ps);
  }
  refresh();
}

void otg_fs::bus_reset(vtime at)
{
  if(not pulled_up())
  {
    return;
  }
  state_.address         = 0;
  state_.address_pending = false;
  state_.suspended       = false;
  state_.in              = {};
  state_.out             = {};
  for(std::size_t n = 1; n < endpoints; ++n)
  {
    update(regs_, in_[n].active, 0);
    update(regs_, out_[n].active, 0);
  }
  update(regs_, suspended_status_, 0);
  update(regs_, bus_reset_, 1);
  schedule(enumeration_done, at.ps + enumeration_ps);
  refresh();
}

void otg_fs::start_of_frame(std::uint16_t frame, vtime at)
{
  if(not pulled_up())
  {
    return;
  }
  state_.frame = frame;
  for(auto& ep : state_.in)
  {
    ep.served = false;
  }
  for(auto& ep : state_.out)
  {
    ep.served = false;
  }
  update(regs_, frame_number_, frame);
  update(regs_, start_of_frame_, 1);
  schedule(periodic_end, at.ps + frame_ps * periodic_percent[get(regs_, periodic_interval_)] / 100);
  refresh();
}

auto otg_fs::setup(std::uint8_t address, usb::setup_packet packet, vtime) -> usb::handshake
{
  if(not on_bus(address))
  {
    return usb::handshake::none;
  }
  auto const& in0  = in_[0];
  auto const& out0 = out_[0];
  update(regs_, in0.stall, 0);
  update(regs_, out0.stall, 0);
  update(regs_, in0.nak, 1);
  update(regs_, in0.nak_effective, 1);
  update(regs_, out0.nak, 1);
  if(auto const count = get(regs_, *out0.setup_count); count > 0)
  {
    update(regs_, *out0.setup_count, count - 1);
  }
  if(rx_used() + 4 > get(regs_, rx_depth_))
  {
    notice(
        "GRXFSIZ", "a SETUP arrived with no room in the receive FIFO: it is kept", setup_overflow);
  }
  auto const bytes = packet.bytes();
  push_rx(status_word(0, bytes.size(), 0, pktsts_setup_data, state_.frame), pack(bytes));
  push_rx(status_word(0, 0, 0, pktsts_setup_done, state_.frame), {});
  state_.in[0].toggle    = 1;
  state_.out[0].toggle   = 1;
  state_.address         = bus_address();
  state_.address_pending = packet.request_type == 0x00 and packet.request == set_address;
  refresh();
  return usb::handshake::ack;
}

auto otg_fs::out(
    std::uint8_t                  address,
    std::uint8_t                  endpoint,
    std::span<std::uint8_t const> data,
    usb::data_pid                 pid,
    vtime) -> usb::handshake
{
  if(not on_bus(address) or endpoint >= endpoints)
  {
    return usb::handshake::none;
  }
  auto const& ep = out_[endpoint];
  if(endpoint != 0 and get(regs_, ep.active) == 0)
  {
    return usb::handshake::none;
  }
  if(endpoint != 0 and get(regs_, ep.type) == isochronous)
  {
    return out_isochronous(endpoint, data);
  }
  if(get(regs_, ep.stall) != 0)
  {
    return usb::handshake::stall;
  }
  if(get(regs_, ep.enable) == 0 or get(regs_, ep.nak) != 0 or get(regs_, out_nak_status_) != 0)
  {
    return usb::handshake::nak;
  }
  auto const largest = out_packet_size(endpoint);
  if(data.size() > largest)
  {
    notice(
        std::format("DOEPCTL{}", endpoint),
        std::format(
            "OUT packet of {} bytes on endpoint {} above its maximum packet size {}: babble",
            data.size(), endpoint, largest),
        babble);
    return usb::handshake::none;
  }
  auto& toggle = state_.out[endpoint].toggle;
  if(static_cast<std::uint8_t>(pid) != toggle)
  {
    return usb::handshake::ack;
  }
  auto const packets   = get(regs_, ep.packets);
  auto const completes = packets <= 1 or data.size() < largest;
  auto const needed    = 1 + words_for(data.size()) + (completes ? 1 : 0);
  if(rx_used() + needed > get(regs_, rx_depth_))
  {
    return usb::handshake::nak;
  }
  push_rx(status_word(endpoint, data.size(), toggle, pktsts_out_data, state_.frame), pack(data));
  toggle ^= 1u;
  auto const size = get(regs_, ep.size);
  update(
      regs_, ep.size,
      size - std::min<std::uint32_t>(size, static_cast<std::uint32_t>(data.size())));
  update(regs_, ep.packets, packets > 0 ? packets - 1 : 0);
  if(completes)
  {
    push_rx(status_word(endpoint, 0, 0, pktsts_out_done, state_.frame), {});
    update(regs_, ep.enable, 0);
    update(regs_, ep.nak, 1);
  }
  refresh();
  return usb::handshake::ack;
}

auto otg_fs::in(std::uint8_t address, std::uint8_t endpoint, vtime) -> usb::in_result
{
  if(not on_bus(address) or endpoint >= endpoints)
  {
    return {};
  }
  auto const& ep = in_[endpoint];
  if(endpoint != 0 and get(regs_, ep.active) == 0)
  {
    return {};
  }
  if(endpoint != 0 and get(regs_, ep.type) == isochronous)
  {
    return in_isochronous(endpoint);
  }
  if(get(regs_, ep.stall) != 0)
  {
    return refused(usb::handshake::stall);
  }
  auto const packets = get(regs_, ep.packets);
  auto const halted  = get(regs_, ep.nak) != 0 or get(regs_, in_nak_status_) != 0;
  if(get(regs_, ep.enable) == 0 or halted or packets == 0)
  {
    return refused(usb::handshake::nak);
  }
  auto& fifo    = state_.tx_fifo[endpoint];
  auto& written = state_.tx_packets[endpoint];
  auto  bytes   = std::size_t{0};
  if(not written.empty())
  {
    bytes = written.front();
    written.erase(written.begin());
  }
  else if(not fifo.empty() or get(regs_, ep.size) != 0)
  {
    return refused(usb::handshake::nak);
  }
  auto const     words = words_for(bytes);
  usb::in_result result{
      .hs   = usb::handshake::ack,
      .pid  = static_cast<usb::data_pid>(state_.in[endpoint].toggle),
      .data = std::vector<std::uint8_t>(bytes)};
  for(std::size_t index = 0; index < bytes; ++index)
  {
    result.data[index] = static_cast<std::uint8_t>(fifo[index / 4] >> (8 * (index % 4)));
  }
  fifo.erase(fifo.begin(), fifo.begin() + static_cast<std::ptrdiff_t>(words));
  state_.in[endpoint].toggle ^= 1u;
  update(regs_, ep.packets, packets - 1);
  if(packets == 1)
  {
    update(regs_, ep.enable, 0);
    update(regs_, ep.complete, 1);
  }
  if(endpoint == 0)
  {
    state_.address_pending = false;
  }
  packetize(endpoint);
  refresh();
  return result;
}

auto otg_fs::frame_parity() const -> std::uint8_t
{
  return static_cast<std::uint8_t>(state_.frame & 1u);
}

auto otg_fs::in_isochronous(std::size_t n) -> usb::in_result
{
  auto const& ep    = in_[n];
  auto&       state = state_.in[n];
  if(get(regs_, ep.enable) == 0 or state.toggle != frame_parity())
  {
    return {};
  }
  auto&          written = state_.tx_packets[n];
  auto&          fifo    = state_.tx_fifo[n];
  usb::in_result result{.hs = usb::handshake::ack, .pid = usb::data_pid::data0, .data = {}};
  auto const     packets = get(regs_, ep.packets);
  if(packets == 0)
  {
    return result;
  }
  auto bytes = std::size_t{0};
  if(not written.empty())
  {
    bytes = written.front();
    written.erase(written.begin());
  }
  else if(not fifo.empty() or get(regs_, ep.size) != 0)
  {
    return result;
  }
  result.data.resize(bytes);
  for(std::size_t index = 0; index < bytes; ++index)
  {
    result.data[index] = static_cast<std::uint8_t>(fifo[index / 4] >> (8 * (index % 4)));
  }
  fifo.erase(fifo.begin(), fifo.begin() + static_cast<std::ptrdiff_t>(words_for(bytes)));
  state.served = true;
  update(regs_, ep.packets, packets - 1);
  if(packets == 1)
  {
    update(regs_, ep.enable, 0);
    update(regs_, ep.complete, 1);
  }
  packetize(n);
  refresh();
  return result;
}

auto otg_fs::out_isochronous(std::size_t n, std::span<std::uint8_t const> data) -> usb::handshake
{
  auto const& ep    = out_[n];
  auto&       state = state_.out[n];
  if(get(regs_, ep.enable) == 0
     or state.toggle != frame_parity()
     or get(regs_, out_nak_status_) != 0)
  {
    return usb::handshake::none;
  }
  auto const largest   = out_packet_size(n);
  auto const packets   = get(regs_, ep.packets);
  auto const completes = packets <= 1 or data.size() < largest;
  auto const needed    = 1 + words_for(data.size()) + (completes ? 1 : 0);
  if(data.size() > largest or rx_used() + needed > get(regs_, rx_depth_))
  {
    notice(
        std::format("DOEPCTL{}", n),
        std::format(
            "iso OUT packet of {} bytes on endpoint {} dropped: above the maximum packet size {} "
            "or no room in the receive FIFO",
            data.size(), n, largest),
        iso_dropped);
    return usb::handshake::none;
  }
  auto const endpoint = static_cast<std::uint32_t>(n);
  push_rx(status_word(endpoint, data.size(), 0, pktsts_out_data, state_.frame), pack(data));
  state.served    = true;
  auto const size = get(regs_, ep.size);
  update(
      regs_, ep.size,
      size - std::min<std::uint32_t>(size, static_cast<std::uint32_t>(data.size())));
  update(regs_, ep.packets, packets > 0 ? packets - 1 : 0);
  if(completes)
  {
    push_rx(status_word(endpoint, 0, 0, pktsts_out_done, state_.frame), {});
    update(regs_, ep.enable, 0);
  }
  refresh();
  return usb::handshake::ack;
}

void otg_fs::end_periodic_frame()
{
  update(regs_, end_of_periodic_, 1);
  auto const parity = frame_parity();
  for(std::size_t n = 1; n < endpoints; ++n)
  {
    auto const pending = [&](auto const& ep, endpoint_state const& state) {
      auto const iso      = get(regs_, ep.type) == isochronous;
      auto const enabled  = get(regs_, ep.enable) != 0;
      auto const in_frame = state.toggle == parity;
      return iso and enabled and in_frame and not state.served;
    };
    if(pending(in_[n], state_.in[n]))
    {
      update(regs_, incomplete_iso_in_, 1);
    }
    if(pending(out_[n], state_.out[n]))
    {
      update(regs_, incomplete_iso_out_, 1);
    }
  }
}

template <typename Fields>
void otg_fs::check_iso_packets(std::string_view reg, Fields const& ep, std::size_t n)
{
  if(n != 0
     and get(regs_, ep.type) == isochronous
     and get(regs_, ep.enable) != 0
     and get(regs_, ep.packets) > 1)
  {
    unmodelled(
        reg,
        "an isochronous endpoint armed with PKTCNT above 1 is not modelled: one packet goes per "
        "frame and the transfer spans frames",
        iso_packets);
  }
}

void otg_fs::check_fifo_ram()
{
  auto total  = std::size_t{get(regs_, rx_depth_)} + tx_depth(0);
  auto active = false;
  for(std::size_t n = 1; n < endpoints; ++n)
  {
    if(get(regs_, in_[n].active) != 0)
    {
      total += tx_depth(n);
      active = true;
    }
  }
  if(active and total > fifo_ram_words)
  {
    notice(
        "GRXFSIZ",
        std::format(
            "the receive FIFO and the TX FIFOs of the active IN endpoints take {} words, above "
            "the {} words of the OTG_FS FIFO RAM",
            total, fifo_ram_words),
        fifo_ram);
  }
}

void otg_fs::suspend(vtime)
{
  if(pulled_up())
  {
    enter_suspend();
  }
}

void otg_fs::enter_suspend()
{
  state_.suspended = true;
  update(regs_, suspended_status_, 1);
  update(regs_, early_suspend_, 1);
  update(regs_, suspend_, 1);
  refresh();
}

void otg_fs::resume(vtime)
{
  if(not state_.suspended)
  {
    return;
  }
  state_.suspended = false;
  update(regs_, suspended_status_, 0);
  update(regs_, wake_up_, 1);
  refresh();
}
}

auto otg_fs_factory(board const& b) -> model_factory
{
  return [hosts = b.usb_hosts](
             model_context& ctx, peripheral_desc const& desc) -> std::shared_ptr<model> {
    auto const found = hosts.find(desc.name);
    return std::make_shared<otg_fs>(
        ctx, desc,
        found == hosts.end() ? std::nullopt : std::optional<usb_host_options>{found->second});
  };
}
}
