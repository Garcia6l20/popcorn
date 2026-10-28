#include "factories.hpp"
#include "frame_signal.hpp"
#include "model_base.hpp"
#include "usb_device_link.hpp"

#include <popcorn/core/errors.hpp>
#include <popcorn/periph/stm32/usb.hpp>
#include <popcorn/periph/usb.hpp>

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
namespace
{
constexpr std::size_t   endpoints     = 8;
constexpr std::uint32_t host_wake     = 0;
constexpr std::uint32_t idle_suspend  = 1;
constexpr std::uint32_t frame_timer   = 2;
constexpr std::uint64_t frame_ps      = 1'000'000'000;
constexpr std::uint64_t sof_window_ps = 1'000'000;
constexpr std::uint8_t  idle_frames   = 3;
constexpr std::uint32_t pma_first     = 0x400;
constexpr std::uint32_t pma_bytes     = 0x800;
constexpr std::uint32_t pma_last      = pma_first + pma_bytes;

constexpr std::uint32_t stat_disabled = 0;
constexpr std::uint32_t stat_stall    = 1;
constexpr std::uint32_t stat_nak      = 2;

constexpr std::uint32_t type_bulk    = 0;
constexpr std::uint32_t type_control = 1;
constexpr std::uint32_t type_iso     = 2;

constexpr std::uint32_t tx_descriptor = 0;
constexpr std::uint32_t rx_descriptor = 1;

constexpr std::array<std::string_view, 4> endpoint_toggles{"STATTX", "DTOGTX", "STATRX", "DTOGRX"};
constexpr std::array<std::string_view, 7> endpoint_clears{
    "VTTX", "VTRX", "NAK", "ERR_TX", "ERR_RX", "THREE_ERR_TX", "THREE_ERR_RX"};
constexpr std::array<std::string_view, 10> status_clears{
    "L1REQ", "ESOF", "SOF", "RST_DCON", "SUSP", "WKUP", "ERR", "PMAOVR", "THR512", "DDISC"};

enum warning : std::uint32_t
{
  host_mode        = 1u << 0,
  battery_charging = 1u << 1,
  link_power       = 1u << 2,
  remote_wakeup    = 1u << 3,
  pma_width        = 1u << 4,
};

struct usb_drd_state
{
  std::array<std::uint32_t, 3> generation{};
  std::vector<std::byte>       pma = std::vector<std::byte>(pma_bytes);
  usb_link_state               link;
  std::uint8_t                 frames{};
  std::uint8_t                 armed{};
  bool                         suspended{};
  bool                         in_reset{};
  bool                         timing{};
  std::uint8_t                 missed{};

  friend auto operator==(usb_drd_state const&, usb_drd_state const&) -> bool = default;
};

struct endpoint_fields
{
  std::uint32_t offset{};
  field_ref     address;
  field_ref     stat_tx;
  field_ref     dtog_tx;
  field_ref     vttx;
  field_ref     kind;
  field_ref     type;
  field_ref     setup;
  field_ref     stat_rx;
  field_ref     dtog_rx;
  field_ref     vtrx;
};

struct interrupt_pair
{
  field_ref flag;
  field_ref mask;
};

[[nodiscard]] auto pid_of(std::uint32_t toggle) -> usb::data_pid
{
  return toggle != 0 ? usb::data_pid::data1 : usb::data_pid::data0;
}

[[nodiscard]] auto refused(usb::handshake hs) -> usb::in_result
{
  return {.hs = hs, .pid = usb::data_pid::data0, .data = {}};
}

[[nodiscard]] auto allocated(std::uint32_t descriptor) -> std::size_t
{
  auto const blocks = (descriptor >> 26) & 0x1fu;
  if((descriptor >> 31) != 0)
  {
    return std::min<std::size_t>((blocks + 1) * 32, 1023);
  }
  return blocks * 2;
}

/**
 * @brief USB full-speed device controller of the STM32H5, CubeMX IP `USB` `usbfs_v1`, in device
 *        mode, RM0481 section 61, and the bus side of the device as a usb::device_port. The U5
 *        OTG_FS core of RM0456 section 72 is the otg_fs model.
 *
 * Registers, RM0481 61.6: CHEPnR keep their bit kinds by name, checked against the description
 * at construction: STATTX, DTOGTX, STATRX and DTOGRX toggle on a written 1, VTTX, VTRX, NAK and
 * the error fields clear on a written 0, SETUP is read-only, the rest is replaced. A byte or
 * half-word write of any register is a word write with the other lanes 0, as measured on an
 * STM32H573 for CHEPnR and DADDR and applied to every register: a toggle bit there stays, a bit
 * cleared by 0 clears and a field reads 0. ISTR's L1REQ to DDISC clear on a written 0, CTR, DIR and
 * IDN follow the endpoints: CTR while one has VTRX or VTTX, IDN the first such endpoint,
 * isochronous and double-buffered bulk ones before the others, lowest number first, DIR 1 when it
 * has VTRX. IRQ 74 is any ISTR flag whose CNTR mask, the flag's name plus M, is set. SUSPRDY reads
 * SUSPEN. A written 0 keeps SUSPEN, as measured on an STM32H573. FNR.RXDP and RXDM read 1 and 1
 * with no host attached, as measured on an STM32H573. With a host attached they read SE0, 0 and 0,
 * while the device does not pull D+ up, the host's pull-downs of USB 2.0 7.1.5.1, and the idle J
 * state, RXDP 1 and RXDM 0, while it does, USB 2.0 7.1.7.1, both as measured on an STM32H573
 * with a PC host.
 *
 * Packet memory, RM0481 61.3 and 61.7: 2048 bytes at offsets 0x400 to 0xBFF, zero at power-on
 * and kept by every other reset. The buffer descriptors of endpoint n are its words 2n and
 * 2n + 1, USB_CHEP_TXRXBD_n and USB_CHEP_RXTXBD_n. Buffer addresses wrap inside the memory, the
 * low two bits are ignored. A receive buffer holds BLSIZE 0 ? 2 * NUM_BLOCK : 32 * (NUM_BLOCK +
 * 1) bytes, at most 1023, RM0481 Table 698.
 *
 * Bus, RM0481 61.5.2: the device pulls D+ up while a host is attached and BCDR.DPPU_DPD is set,
 * and tells the host of every change. It answers tokens to DADDR.ADD while DADDR.EF is set and
 * CNTR.PDWN, CNTR.USBRST and ISTR.RST_DCON are clear, RM0481 61.6.1, on the first endpoint whose
 * EA matches with its STAT for the direction not DISABLED. A bus reset clears every CHEPnR but
 * VTRX and VTTX and DADDR when SE0 starts, inferred. It clears SUSPEN and SUSPRDY when SE0 starts
 * and sets ISTR.RST_DCON when SE0 ends, as measured on an STM32H573 with a PC host. CNTR.USBRST
 * written from 0 to 1 clears every CHEPnR but VTRX and VTTX, RM0481 61.6.1 and 61.6.7, keeps
 * DADDR, as measured on an STM32H573, and leaves RST_DCON as it is. A SOF sets FNR.FN, clears
 * LSOF and raises ISTR.SOF. FNR.LCK sets at the second SOF after a bus reset, with
 * ISTR.RST_DCON pending or cleared, and reads 0 while ISTR.WKUP is set, as measured on an
 * STM32H573 with a PC host after a bus reset. After a resume it sets at the second SOF, RM0481
 * FNR.LCK, and reads 0 while WKUP is set, inferred. A SOF while WKUP is set starts the count of
 * two again, so LCK sets at the second SOF after the firmware clears WKUP, inferred: on the part
 * WKUP was cleared before SE0 ended. Suspend raises SUSP, resume raises WKUP and clears SUSPEN. A
 * bus reset or a SOF while CNTR.SUSPEN is set raises WKUP and clears SUSPEN and SUSPRDY, RM0481
 * 61.5.7 and 61.6.2, the bus reset as read on an STM32H573 with a PC host.
 *
 * Transfers, RM0481 61.5.2: a SETUP to a control endpoint 0 whose STATRX is not DISABLED and
 * whose VTRX is clear lands in its receive buffer, sets COUNT_RX 8, DTOGRX and DTOGTX 1, both
 * STATs NAK, SETUP and VTRX, and is acknowledged. Both toggles 1 follow USB 2.0 8.5.3, the data
 * and status stages of a control transfer start with DATA1, where RM0481 61.5.2 gives DTOGRX 0.
 * With VTRX set it gets no answer. An OUT with STATRX VALID and the expected DTOGRX lands in the
 * receive buffer, sets COUNT_RX, toggles DTOGRX, sets STATRX NAK and VTRX, clears SETUP, and is
 * acknowledged. Another toggle is acknowledged and dropped. An OUT with data to a control endpoint
 * with EPKIND, STATUS_OUT, or one longer than the buffer, is STALLed: the bytes that fit are
 * written, nothing else changes. An IN with STATTX VALID sends COUNT_TX bytes of the transmit
 * buffer with DTOGTX as its data PID, toggles DTOGTX, sets STATTX NAK and VTTX. STALL and NAK
 * answer as their STAT.
 *
 * Double-buffered bulk, EPKIND on a bulk endpoint, RM0481 61.5.3: the DTOG of the direction picks
 * the buffer the same way in both directions, DTOG 0 descriptor word 2n, 1 word 2n + 1, with the
 * descriptor fields of the direction. This follows the 61.5.3 text, "the relationship between
 * the buffer flag value and the used packet buffer is the same in both cases", the ST HAL of
 * this IP and tinyusb. The receive rows of RM0481 Table 687 and Table 689 give the opposite. The
 * other DTOG is SW_BUF. With DTOG equal to SW_BUF the token is NAKed. A transfer toggles DTOG.
 * The first transaction after EPKIND or UTYPE changes, or after a reset, sets STAT NAK as a
 * single-buffered one does, every later one keeps STAT VALID. The change rule is the model's
 * reading of RM0481 61.5.3, which names only DBL_BUF being set.
 *
 * Isochronous, UTYPE 2, RM0481 61.5.5 Table 689: every STAT other than DISABLED answers. Data are
 * DATA0 and no toggle is checked. With EPKIND clear, SBUF_ISO off, the DTOG picks the buffer as
 * for double-buffered bulk and toggles after each packet. With EPKIND set the single buffer of
 * the direction serves. An OUT longer than the buffer keeps the bytes that fit and counts them.
 * Each packet sets VTRX or VTTX and keeps STAT.
 *
 * Host: as usb_device_link describes, RCC USBFSRST included, which drops the pull-up and the
 * pending events and keeps the packet memory.
 *
 * Not modelled, logged once and a model_error under strict_peripherals: host mode CNTR.HOST,
 * LPM LPMCSR.LPMEN, battery charging detection BCDR.BCDEN, remote wake-up CNTR.L1RES and L2RES,
 * a CPU access to the packet memory of another width than a word, which RM0481 61.7 forbids. Such a
 * write stores its lanes and zeroes the others, a read returns its lanes, as measured on an
 * STM32H573.
 *
 * Frame timer: while BCDR.DPPU_DPD is set and CNTR.PDWN, USBRST and SUSPEN are clear, with a host
 * attached or not, ISTR.ESOF rises and FNR.LSOF counts, wrapping at 4, every 1 ms with no SOF.
 * SUSP rises at each ESOF that brings the count of missed frames to 3, the count wrapping at 4 and
 * starting again at a SOF, a bus reset, a resume and the timer start, so with no SOF it rises
 * every 4 ms, as read on an STM32H573 with nothing attached and with a PC host before its bus
 * reset. Setting SUSPEN stops the timer and clears LSOF, as read on the part, RM0481 61.5.7. The
 * first ESOF comes 1 ms after the pull-up and each SOF moves the next one to 1 ms plus 1 us after
 * it. A bus reset stops the timer while SE0 lasts, as read on an STM32H573 with a PC host after
 * a reset with and without suspend, and the next ESOF comes 1 ms after SE0 ends, as RM0481 61.6.2
 * enables the suspend check right after any USB reset. The 1 ms period follows RM0481 61.6.2. The
 * first ESOF time and the 1 us window are inferred.
 * Clearing DPPU_DPD stops the timer, and FN, LCK, LSOF and the ISTR flags keep their values, as
 * read on the part with a PC host.
 */
class usb_drd final : public dispatch_model<usb_drd, usb_drd_state>,
                      public usb_device_link<usb_drd>,
                      public bus_reset_listener
{
  friend usb_device_link<usb_drd>;

public:
  static constexpr std::uint32_t host_wake_event    = host_wake;
  static constexpr std::uint32_t idle_suspend_event = idle_suspend;

  usb_drd(model_context& ctx, peripheral_desc const& desc, std::optional<usb_host_options> wired);

  [[nodiscard]] auto served_ranges() const
      -> std::vector<std::pair<std::uint32_t, std::uint32_t>> override;
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

  [[nodiscard]] auto pulled_up() const -> bool override;
  void               bus_reset(vtime at) override;
  void               bus_reset_end(vtime at) override;
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
  [[nodiscard]] auto now() const -> vtime override;

  void plain_written(std::uint32_t old, std::uint32_t now);
  void endpoint_written(std::uint32_t old, std::uint32_t now);
  void control_written(std::uint32_t old, std::uint32_t now);
  void status_written(std::uint32_t old, std::uint32_t now);
  void link_power_written(std::uint32_t old, std::uint32_t now);
  void charging_written(std::uint32_t old, std::uint32_t now);

private:
  [[nodiscard]] auto endpoint_fields_of(std::size_t n) const -> endpoint_fields;
  [[nodiscard]] auto kind_mask(std::span<std::string_view const> names, write_effect effect) const
      -> std::uint32_t;
  [[nodiscard]] auto
      applied(std::uint32_t old, std::uint32_t now, std::uint32_t clears, std::uint32_t toggles)
          const -> std::uint32_t;
  [[nodiscard]] auto on_bus(std::uint8_t address) const -> bool;
  [[nodiscard]] auto find(std::uint8_t endpoint, field_ref endpoint_fields::* stat) const
      -> std::optional<std::size_t>;
  [[nodiscard]] auto descriptor(std::size_t n, std::uint32_t which) const -> std::uint32_t;
  void               set_count(std::size_t n, std::uint32_t which, std::size_t bytes);
  [[nodiscard]] auto load_bytes(std::uint32_t descriptor) const -> std::vector<std::uint8_t>;
  void               store_bytes(std::uint32_t descriptor, std::span<std::uint8_t const> bytes);
  [[nodiscard]] auto pma_word(std::uint32_t at) const -> std::uint32_t;
  void               set_pma_word(std::uint32_t at, std::uint32_t word);
  void               restore_reset_values(std::optional<reset_kind> kind);
  void               clear_endpoints(bool address);
  [[nodiscard]] auto first_transaction(std::size_t n) -> bool;
  void               link(bool report);
  void               refresh();
  void               schedule(std::uint32_t kind, std::uint64_t at_ps);
  void               cancel(std::uint32_t kind);
  void               enter_suspend();
  void               raise_suspend();
  void               wake();
  void               time_frames();
  void               frame_missed();
  void unmodelled(std::string_view name, std::uint32_t offset, std::string message, warning kind);
  [[nodiscard]] auto link_state() -> usb_link_state&;
  [[nodiscard]] auto link_state() const -> usb_link_state const&;

  register_file&                         regs_;
  frame_output                           frames_;
  irq_line&                              line_;
  std::array<endpoint_fields, endpoints> ep_;
  std::uint32_t                          endpoint_clears_{};
  std::uint32_t                          endpoint_toggles_{};
  std::uint32_t                          status_clears_{};
  std::vector<interrupt_pair>            interrupts_;
  field_ref                              transfer_;
  field_ref                              direction_;
  field_ref                              endpoint_id_;
  field_ref                              reset_done_;
  field_ref                              frame_flag_;
  field_ref                              suspend_flag_;
  field_ref                              expected_flag_;
  field_ref                              wake_flag_;
  field_ref                              core_reset_;
  field_ref                              power_down_;
  field_ref                              suspend_enable_;
  field_ref                              suspend_ready_;
  field_ref                              l1_resume_;
  field_ref                              l2_resume_;
  field_ref                              host_mode_;
  field_ref                              frame_number_;
  field_ref                              lost_frames_;
  field_ref                              locked_;
  field_ref                              line_plus_;
  field_ref                              line_minus_;
  field_ref                              address_;
  field_ref                              function_enable_;
  field_ref                              lpm_enable_;
  field_ref                              charging_enable_;
  field_ref                              pull_up_;
  std::uint32_t                          access_offset_{};
  std::uint32_t                          warned_{};
};

constexpr std::array<write_entry<usb_drd>, 14> usb_drd_writes{
    {
     {"CHEP0R", &usb_drd::endpoint_written},
     {"CHEP1R", &usb_drd::endpoint_written},
     {"CHEP2R", &usb_drd::endpoint_written},
     {"CHEP3R", &usb_drd::endpoint_written},
     {"CHEP4R", &usb_drd::endpoint_written},
     {"CHEP5R", &usb_drd::endpoint_written},
     {"CHEP6R", &usb_drd::endpoint_written},
     {"CHEP7R", &usb_drd::endpoint_written},
     {"CNTR", &usb_drd::control_written},
     {"ISTR", &usb_drd::status_written},
     {"FNR", &usb_drd::plain_written},
     {"DADDR", &usb_drd::plain_written},
     {"LPMCSR", &usb_drd::link_power_written},
     {"BCDR", &usb_drd::charging_written},
     }
};

usb_drd::usb_drd(
    model_context&                  ctx,
    peripheral_desc const&          desc,
    std::optional<usb_host_options> wired)
    : dispatch_model{ctx, desc, "USB", usb_drd_writes}, regs_{ctx.regs}, frames_{ctx.peers},
      line_{irq("USB_DRD_FS_IRQn")}, ep_{[this]<std::size_t... n>(std::index_sequence<n...>) {
        return std::array<endpoint_fields, endpoints>{endpoint_fields_of(n)...};
      }(std::make_index_sequence<endpoints>{})},
      transfer_{field("ISTR", "CTR")}, direction_{field("ISTR", "DIR")},
      endpoint_id_{field("ISTR", "IDN")}, reset_done_{field("ISTR", "RST_DCON")},
      frame_flag_{field("ISTR", "SOF")}, suspend_flag_{field("ISTR", "SUSP")},
      expected_flag_{field("ISTR", "ESOF")}, wake_flag_{field("ISTR", "WKUP")},
      core_reset_{field("CNTR", "USBRST")}, power_down_{field("CNTR", "PDWN")},
      suspend_enable_{field("CNTR", "SUSPEN")}, suspend_ready_{field("CNTR", "SUSPRDY")},
      l1_resume_{field("CNTR", "L1RES")}, l2_resume_{field("CNTR", "L2RES")},
      host_mode_{field("CNTR", "HOST")}, frame_number_{field("FNR", "FN")},
      lost_frames_{field("FNR", "LSOF")}, locked_{field("FNR", "LCK")},
      line_plus_{field("FNR", "RXDP")}, line_minus_{field("FNR", "RXDM")},
      address_{field("DADDR", "ADD")}, function_enable_{field("DADDR", "EF")},
      lpm_enable_{field("LPMCSR", "LPMEN")}, charging_enable_{field("BCDR", "BCDEN")},
      pull_up_{field("BCDR", "DPPU_DPD")}
{
  endpoint_toggles_ = kind_mask(endpoint_toggles, write_effect::one_to_toggle);
  endpoint_clears_  = kind_mask(endpoint_clears, write_effect::zero_to_clear);
  for(auto const name : status_clears)
  {
    auto const& found = std::ranges::find(describe("ISTR").fields, name, &field_desc::name);
    if(found == describe("ISTR").fields.end() or found->on_write != write_effect::zero_to_clear)
    {
      throw config_error(
          ctx_.part.part, 0,
          std::format("peripherals.{}.registers.ISTR.fields.{}.on_write", desc_.name, name),
          "zero_to_clear, rc_w0 in RM0481 61.6.2");
    }
    status_clears_ |= field("ISTR", name).mask();
  }
  for(auto const& flag : describe("ISTR").fields)
  {
    auto const enable = flag.name + "M";
    if(std::ranges::contains(describe("CNTR").fields, enable, &field_desc::name))
    {
      interrupts_.push_back({.flag = field("ISTR", flag.name), .mask = field("CNTR", enable)});
    }
  }
  wire(std::move(wired));
}

auto usb_drd::endpoint_fields_of(std::size_t n) const -> endpoint_fields
{
  auto const name = std::format("CHEP{}R", n);
  return {
      .offset  = reg(name).offset,
      .address = field(name, "EA"),
      .stat_tx = field(name, "STATTX"),
      .dtog_tx = field(name, "DTOGTX"),
      .vttx    = field(name, "VTTX"),
      .kind    = field(name, "EPKIND"),
      .type    = field(name, "UTYPE"),
      .setup   = field(name, "SETUP"),
      .stat_rx = field(name, "STATRX"),
      .dtog_rx = field(name, "DTOGRX"),
      .vtrx    = field(name, "VTRX")};
}

auto usb_drd::kind_mask(std::span<std::string_view const> names, write_effect effect) const
    -> std::uint32_t
{
  std::uint32_t mask = 0;
  for(std::size_t n = 0; n < endpoints; ++n)
  {
    auto const  reg_name = std::format("CHEP{}R", n);
    auto const& fields   = describe(reg_name).fields;
    for(auto const name : names)
    {
      auto const found = std::ranges::find(fields, name, &field_desc::name);
      if(found == fields.end()
         or found->on_write != effect
         or found->acc != popcorn::access::read_write)
      {
        throw config_error(
            ctx_.part.part, 0,
            std::format(
                "peripherals.{}.registers.{}.fields.{}.on_write", desc_.name, reg_name, name),
            std::format("{}, read_write, the bit kind of RM0481 61.6.7", effect));
      }
      mask |= field(reg_name, name).mask();
    }
  }
  return mask;
}

auto usb_drd::served_ranges() const -> std::vector<std::pair<std::uint32_t, std::uint32_t>>
{
  return {
      {pma_first, pma_last}
  };
}

auto usb_drd::link_state() -> usb_link_state&
{
  return state_.link;
}

auto usb_drd::link_state() const -> usb_link_state const&
{
  return state_.link;
}

void usb_drd::reset(register_file&, reset_kind kind)
{
  restore_reset_values(kind);
}

void usb_drd::on_bus_reset()
{
  restore_reset_values(std::nullopt);
}

void usb_drd::restore_reset_values(std::optional<reset_kind> kind)
{
  for(auto const& name : claims())
  {
    regs_.poke(reg(name).offset, describe(name).reset);
  }
  auto const generation = state_.generation;
  auto const kept       = state_.link;
  auto       memory     = std::move(state_.pma);
  state_                = {};
  state_.generation     = generation;
  for(auto& count : state_.generation)
  {
    ++count;
  }
  if(kind != reset_kind::power_on)
  {
    state_.pma = std::move(memory);
  }
  keep_link(kept, kind);
  link(false);
  refresh();
}

auto usb_drd::now() const -> vtime
{
  return ctx_.clock.time();
}

auto usb_drd::pulled_up() const -> bool
{
  return host() != nullptr and get(regs_, pull_up_) != 0;
}

auto usb_drd::on_bus(std::uint8_t address) const -> bool
{
  return pulled_up()
     and get(regs_, function_enable_) != 0
     and get(regs_, power_down_) == 0
     and get(regs_, core_reset_) == 0
     and get(regs_, reset_done_) == 0
     and get(regs_, address_) == address;
}

void usb_drd::link(bool)
{
  tell_pull_up(pulled_up());
}

void usb_drd::time_frames()
{
  if(not pulled_up())
  {
    state_.in_reset = false;
  }
  auto const running = get(regs_, pull_up_) != 0
                   and get(regs_, power_down_) == 0
                   and get(regs_, core_reset_) == 0
                   and get(regs_, suspend_enable_) == 0
                   and not state_.in_reset;
  if(running == state_.timing)
  {
    return;
  }
  state_.timing = running;
  state_.missed = 0;
  if(running)
  {
    schedule(frame_timer, now().ps + frame_ps);
  }
  else
  {
    cancel(frame_timer);
  }
}

void usb_drd::frame_missed()
{
  update(regs_, expected_flag_, 1);
  auto const wrap = 1u << lost_frames_.width;
  update(regs_, lost_frames_, (get(regs_, lost_frames_) + 1u) % wrap);
  state_.missed = static_cast<std::uint8_t>((state_.missed + 1u) % wrap);
  if(state_.missed == idle_frames)
  {
    raise_suspend();
  }
  schedule(frame_timer, now().ps + frame_ps);
}

void usb_drd::refresh()
{
  time_frames();
  auto const attached = host() != nullptr;
  update(regs_, line_plus_, attached and not pulled_up() ? 0u : 1u);
  update(regs_, line_minus_, attached ? 0u : 1u);
  std::optional<std::size_t> first;
  for(auto const priority : {true, false})
  {
    for(std::size_t n = 0; n < endpoints and not first; ++n)
    {
      auto const word     = regs_.peek(ep_[n].offset);
      auto const type     = ep_[n].type.of(word);
      auto const buffered = type == type_iso or (type == type_bulk and ep_[n].kind.of(word) != 0);
      auto const pending  = ep_[n].vtrx.of(word) != 0 or ep_[n].vttx.of(word) != 0;
      if(pending and buffered == priority)
      {
        first = n;
      }
    }
  }
  update(regs_, transfer_, first ? 1 : 0);
  update(regs_, endpoint_id_, first ? static_cast<std::uint32_t>(*first) : 0);
  update(regs_, direction_, first ? ep_[*first].vtrx.of(regs_.peek(ep_[*first].offset)) : 0);
  auto const raised = std::ranges::any_of(interrupts_, [this](interrupt_pair const& pair) {
    return get(regs_, pair.flag) != 0 and get(regs_, pair.mask) != 0;
  });
  if(raised)
  {
    line_.raise();
  }
  else
  {
    line_.lower();
  }
}

void usb_drd::schedule(std::uint32_t kind, std::uint64_t at_ps)
{
  ctx_.events.schedule_at(
      vtime{std::max(at_ps, now().ps)}, kind, ++state_.generation[kind], ctx_.clock);
}

void usb_drd::cancel(std::uint32_t kind)
{
  ++state_.generation[kind];
}

void usb_drd::unmodelled(
    std::string_view name,
    std::uint32_t    offset,
    std::string      message,
    warning          kind)
{
  if((warned_ & kind) == 0)
  {
    warned_ |= kind;
    warn(name, std::move(message));
  }
  if(ctx_.strict_peripherals)
  {
    throw model_error(desc_.name, std::string{name}, offset);
  }
}

auto usb_drd::pma_word(std::uint32_t at) const -> std::uint32_t
{
  auto const    base = at & (pma_bytes - 4);
  std::uint32_t word = 0;
  for(std::uint32_t lane = 0; lane < 4; ++lane)
  {
    word |= std::to_integer<std::uint32_t>(state_.pma[base + lane]) << (8 * lane);
  }
  return word;
}

void usb_drd::set_pma_word(std::uint32_t at, std::uint32_t word)
{
  auto const base = at & (pma_bytes - 4);
  for(std::uint32_t lane = 0; lane < 4; ++lane)
  {
    state_.pma[base + lane] = static_cast<std::byte>(word >> (8 * lane));
  }
}

auto usb_drd::descriptor(std::size_t n, std::uint32_t which) const -> std::uint32_t
{
  return pma_word(static_cast<std::uint32_t>(8 * n + 4 * which));
}

void usb_drd::set_count(std::size_t n, std::uint32_t which, std::size_t bytes)
{
  auto const at   = static_cast<std::uint32_t>(8 * n + 4 * which);
  auto const word = pma_word(at);
  set_pma_word(at, (word & ~0x03ff'0000u) | ((static_cast<std::uint32_t>(bytes) & 0x3ffu) << 16));
}

auto usb_drd::load_bytes(std::uint32_t descriptor) const -> std::vector<std::uint8_t>
{
  auto const                count = (descriptor >> 16) & 0x3ffu;
  auto const                start = descriptor & 0xfffcu;
  std::vector<std::uint8_t> bytes(count);
  for(std::uint32_t index = 0; index < count; ++index)
  {
    bytes[index] = std::to_integer<std::uint8_t>(state_.pma[(start + index) % pma_bytes]);
  }
  return bytes;
}

void usb_drd::store_bytes(std::uint32_t descriptor, std::span<std::uint8_t const> bytes)
{
  auto const start = descriptor & 0xfffcu;
  for(std::size_t index = 0; index < bytes.size(); ++index)
  {
    state_.pma[(start + index) % pma_bytes] = static_cast<std::byte>(bytes[index]);
  }
}

auto usb_drd::on_read(register_file& regs, std::uint32_t offset, std::uint8_t size, access_ctx ctx)
    -> std::expected<std::uint32_t, bus_error>
{
  if(offset >= pma_first and offset < pma_last)
  {
    if(size != 4 and ctx.by != initiator::debugger)
    {
      unmodelled(
          "USBSRAM", offset & ~3u,
          std::format("a {}-byte read of the packet memory reads its lanes of the word", size),
          pma_width);
    }
    return read_lanes(pma_word(offset - pma_first), offset, size);
  }
  return dispatch_model::on_read(regs, offset, size, ctx);
}

auto usb_drd::on_write(
    register_file& regs,
    std::uint32_t  offset,
    std::uint8_t   size,
    std::uint32_t  old,
    std::uint32_t  value,
    access_ctx     ctx) -> std::expected<void, bus_error>
{
  if(offset >= pma_first and offset < pma_last)
  {
    if(size != 4 and ctx.by != initiator::debugger)
    {
      unmodelled(
          "USBSRAM", offset & ~3u,
          std::format(
              "a {}-byte write of the packet memory zeroes the other lanes of the word", size),
          pma_width);
    }
    set_pma_word(offset - pma_first, merge_write(0, offset, size, value));
    return {};
  }
  access_offset_ = offset & ~3u;
  return dispatch_model::on_write(
      regs, access_offset_, 4, old, merge_write(0, offset, size, value), ctx);
}

auto usb_drd::applied(
    std::uint32_t old,
    std::uint32_t now,
    std::uint32_t clears,
    std::uint32_t toggles) const -> std::uint32_t
{
  return (now & ~(clears | toggles)) | (old & now & clears) | ((old ^ now) & toggles);
}

void usb_drd::plain_written(std::uint32_t, std::uint32_t)
{}

void usb_drd::endpoint_written(std::uint32_t old, std::uint32_t now)
{
  for(std::size_t n = 0; n < endpoints; ++n)
  {
    if(ep_[n].offset == access_offset_
       and ((old ^ now) & (ep_[n].kind.mask() | ep_[n].type.mask())) != 0)
    {
      state_.armed &= static_cast<std::uint8_t>(~(1u << n));
    }
  }
  regs_.poke(access_offset_, applied(old, now, endpoint_clears_, endpoint_toggles_));
  refresh();
}

void usb_drd::status_written(std::uint32_t old, std::uint32_t now)
{
  regs_.poke(access_offset_, applied(old, now, status_clears_, 0));
  refresh();
}

void usb_drd::control_written(std::uint32_t old, std::uint32_t now)
{
  if(host_mode_.of(now) != 0)
  {
    unmodelled("CNTR", access_offset_, "host mode, CNTR.HOST, is not modelled", host_mode);
  }
  auto const resume_bits = l1_resume_.mask() | l2_resume_.mask();
  if((now & resume_bits & ~old) != 0)
  {
    unmodelled(
        "CNTR", access_offset_, "remote wake-up signalling, CNTR.L1RES and L2RES, is not modelled",
        remote_wakeup);
  }
  if(core_reset_.of(now) != 0 and core_reset_.of(old) == 0)
  {
    clear_endpoints(false);
  }
  if(suspend_enable_.of(old) != 0)
  {
    update(regs_, suspend_enable_, 1);
  }
  else if(suspend_enable_.of(now) != 0)
  {
    update(regs_, lost_frames_, 0);
  }
  update(regs_, suspend_ready_, get(regs_, suspend_enable_));
  refresh();
}

void usb_drd::link_power_written(std::uint32_t, std::uint32_t now)
{
  if(lpm_enable_.of(now) != 0)
  {
    unmodelled("LPMCSR", access_offset_, "LPM, LPMCSR.LPMEN, is not modelled", link_power);
  }
}

void usb_drd::charging_written(std::uint32_t, std::uint32_t now)
{
  if(charging_enable_.of(now) != 0)
  {
    unmodelled(
        "BCDR", access_offset_, "battery charging detection, BCDR.BCDEN, is not modelled",
        battery_charging);
  }
  link(true);
  refresh();
}

void usb_drd::on_event(std::uint32_t kind, std::uint64_t payload)
{
  if(kind >= state_.generation.size() or payload != state_.generation[kind])
  {
    return;
  }
  if(kind == idle_suspend)
  {
    idle_ended();
  }
  else if(kind == frame_timer)
  {
    frame_missed();
  }
  else
  {
    host_woken();
    return;
  }
  refresh();
}

void usb_drd::save(std::vector<std::byte>& out) const
{
  save_with_host(state_, out);
}

void usb_drd::load(std::span<std::byte const> in)
{
  dispatch_model::load(in);
  state_.pma.resize(pma_bytes);
  loaded();
}

void usb_drd::bus_reset(vtime)
{
  if(not pulled_up())
  {
    return;
  }
  clear_endpoints(true);
  wake();
  update(regs_, locked_, 0);
  state_.frames    = 0;
  state_.suspended = false;
  state_.in_reset  = true;
  refresh();
}

void usb_drd::bus_reset_end(vtime at)
{
  if(not state_.in_reset or not pulled_up())
  {
    return;
  }
  state_.in_reset = false;
  update(regs_, reset_done_, 1);
  time_frames();
  state_.missed = 0;
  if(state_.timing)
  {
    schedule(frame_timer, at.ps + frame_ps);
  }
  refresh();
}

void usb_drd::clear_endpoints(bool address)
{
  for(auto const& ep : ep_)
  {
    regs_.poke(ep.offset, regs_.peek(ep.offset) & (ep.vtrx.mask() | ep.vttx.mask()));
  }
  state_.armed = 0;
  if(address)
  {
    update(regs_, address_, 0);
    update(regs_, function_enable_, 0);
  }
}

auto usb_drd::first_transaction(std::size_t n) -> bool
{
  auto const bit = static_cast<std::uint8_t>(1u << n);
  auto const was = (state_.armed & bit) == 0;
  state_.armed |= bit;
  return was;
}

void usb_drd::start_of_frame(std::uint16_t frame, vtime at)
{
  if(not pulled_up())
  {
    return;
  }
  frames_.start(at.ps);
  update(regs_, frame_number_, frame);
  update(regs_, lost_frames_, 0);
  update(regs_, frame_flag_, 1);
  wake();
  state_.frames = get(regs_, wake_flag_) != 0
                    ? std::uint8_t{0}
                    : static_cast<std::uint8_t>(std::min(state_.frames + 1, 2));
  update(regs_, locked_, state_.frames >= 2 ? 1 : 0);
  time_frames();
  state_.missed    = 0;
  state_.suspended = false;
  if(state_.timing)
  {
    schedule(frame_timer, at.ps + frame_ps + sof_window_ps);
  }
  refresh();
}

auto usb_drd::find(std::uint8_t endpoint, field_ref endpoint_fields::* stat) const
    -> std::optional<std::size_t>
{
  for(std::size_t n = 0; n < endpoints; ++n)
  {
    auto const word = regs_.peek(ep_[n].offset);
    if(ep_[n].address.of(word) == endpoint and (ep_[n].*stat).of(word) != stat_disabled)
    {
      return n;
    }
  }
  return std::nullopt;
}

auto usb_drd::setup(std::uint8_t address, usb::setup_packet packet, vtime) -> usb::handshake
{
  if(not on_bus(address))
  {
    return usb::handshake::none;
  }
  std::optional<std::size_t> found;
  for(std::size_t n = 0; n < endpoints and not found; ++n)
  {
    auto const word = regs_.peek(ep_[n].offset);
    if(ep_[n].address.of(word) == 0
       and ep_[n].type.of(word) == type_control
       and ep_[n].stat_rx.of(word) != stat_disabled)
    {
      found = n;
    }
  }
  if(not found or ep_[*found].vtrx.of(regs_.peek(ep_[*found].offset)) != 0)
  {
    return usb::handshake::none;
  }
  auto const  n      = *found;
  auto const& ep     = ep_[n];
  auto const  buffer = descriptor(n, rx_descriptor);
  auto const  bytes  = packet.bytes();
  auto const  room   = std::min(allocated(buffer), bytes.size());
  store_bytes(buffer, std::span{bytes}.first(room));
  if(room < bytes.size())
  {
    return usb::handshake::stall;
  }
  set_count(n, rx_descriptor, bytes.size());
  auto word = regs_.peek(ep.offset);
  word      = ep.stat_rx.with(word, stat_nak);
  word      = ep.stat_tx.with(word, stat_nak);
  word      = ep.dtog_rx.with(word, 1);
  word      = ep.dtog_tx.with(word, 1);
  word      = ep.setup.with(word, 1);
  word      = ep.vtrx.with(word, 1);
  regs_.poke(ep.offset, word);
  refresh();
  return usb::handshake::ack;
}

auto usb_drd::out(
    std::uint8_t                  address,
    std::uint8_t                  endpoint,
    std::span<std::uint8_t const> data,
    usb::data_pid                 pid,
    vtime) -> usb::handshake
{
  if(not on_bus(address))
  {
    return usb::handshake::none;
  }
  auto const found = find(endpoint, &endpoint_fields::stat_rx);
  if(not found)
  {
    return usb::handshake::none;
  }
  auto const  n      = *found;
  auto const& ep     = ep_[n];
  auto        word   = regs_.peek(ep.offset);
  auto const  type   = ep.type.of(word);
  auto const  kind   = ep.kind.of(word) != 0;
  auto const  toggle = ep.dtog_rx.of(word);
  if(type == type_iso)
  {
    auto const which  = kind or toggle != 0 ? rx_descriptor : tx_descriptor;
    auto const buffer = descriptor(n, which);
    auto const room   = std::min(allocated(buffer), data.size());
    store_bytes(buffer, data.first(room));
    set_count(n, which, room);
    if(not kind)
    {
      word = ep.dtog_rx.with(word, toggle ^ 1u);
    }
    regs_.poke(ep.offset, ep.vtrx.with(word, 1));
    refresh();
    return usb::handshake::ack;
  }
  auto const stat = ep.stat_rx.of(word);
  if(stat == stat_stall)
  {
    return usb::handshake::stall;
  }
  if(stat == stat_nak)
  {
    return usb::handshake::nak;
  }
  auto const buffered = type == type_bulk and kind;
  if(buffered and toggle == ep.dtog_tx.of(word))
  {
    return usb::handshake::nak;
  }
  if(type == type_control and kind and not data.empty())
  {
    return usb::handshake::stall;
  }
  if(pid != pid_of(toggle))
  {
    return usb::handshake::ack;
  }
  auto const which  = buffered and toggle == 0 ? tx_descriptor : rx_descriptor;
  auto const buffer = descriptor(n, which);
  auto const room   = std::min(allocated(buffer), data.size());
  store_bytes(buffer, data.first(room));
  if(room < data.size())
  {
    return usb::handshake::stall;
  }
  set_count(n, which, data.size());
  word = ep.dtog_rx.with(word, toggle ^ 1u);
  if(not buffered or first_transaction(n))
  {
    word = ep.stat_rx.with(word, stat_nak);
  }
  if(ep.vtrx.of(word) == 0)
  {
    word = ep.setup.with(word, 0);
  }
  regs_.poke(ep.offset, ep.vtrx.with(word, 1));
  refresh();
  return usb::handshake::ack;
}

auto usb_drd::in(std::uint8_t address, std::uint8_t endpoint, vtime) -> usb::in_result
{
  if(not on_bus(address))
  {
    return refused(usb::handshake::none);
  }
  auto const found = find(endpoint, &endpoint_fields::stat_tx);
  if(not found)
  {
    return refused(usb::handshake::none);
  }
  auto const  n      = *found;
  auto const& ep     = ep_[n];
  auto        word   = regs_.peek(ep.offset);
  auto const  type   = ep.type.of(word);
  auto const  kind   = ep.kind.of(word) != 0;
  auto const  toggle = ep.dtog_tx.of(word);
  if(type == type_iso)
  {
    auto const which = kind or toggle == 0 ? tx_descriptor : rx_descriptor;
    auto       data  = load_bytes(descriptor(n, which));
    if(not kind)
    {
      word = ep.dtog_tx.with(word, toggle ^ 1u);
    }
    regs_.poke(ep.offset, ep.vttx.with(word, 1));
    refresh();
    return {.hs = usb::handshake::ack, .pid = usb::data_pid::data0, .data = std::move(data)};
  }
  auto const stat = ep.stat_tx.of(word);
  if(stat == stat_stall)
  {
    return refused(usb::handshake::stall);
  }
  if(stat == stat_nak)
  {
    return refused(usb::handshake::nak);
  }
  auto const buffered = type == type_bulk and kind;
  if(buffered and toggle == ep.dtog_rx.of(word))
  {
    return refused(usb::handshake::nak);
  }
  auto const which = buffered and toggle != 0 ? rx_descriptor : tx_descriptor;
  auto       data  = load_bytes(descriptor(n, which));
  word             = ep.dtog_tx.with(word, toggle ^ 1u);
  if(not buffered or first_transaction(n))
  {
    word = ep.stat_tx.with(word, stat_nak);
  }
  regs_.poke(ep.offset, ep.vttx.with(word, 1));
  refresh();
  return {.hs = usb::handshake::ack, .pid = pid_of(toggle), .data = std::move(data)};
}

void usb_drd::suspend(vtime)
{
  if(pulled_up())
  {
    enter_suspend();
  }
}

void usb_drd::enter_suspend()
{
  if(not state_.suspended)
  {
    raise_suspend();
  }
}

void usb_drd::raise_suspend()
{
  state_.suspended = true;
  state_.frames    = 0;
  update(regs_, locked_, 0);
  update(regs_, suspend_flag_, 1);
  refresh();
}

void usb_drd::wake()
{
  if(get(regs_, suspend_enable_) != 0)
  {
    update(regs_, wake_flag_, 1);
  }
  update(regs_, suspend_enable_, 0);
  update(regs_, suspend_ready_, 0);
}

void usb_drd::resume(vtime)
{
  if(not state_.suspended)
  {
    return;
  }
  state_.suspended = false;
  state_.missed    = 0;
  update(regs_, suspend_enable_, 0);
  update(regs_, suspend_ready_, 0);
  update(regs_, wake_flag_, 1);
  refresh();
}
}

auto usb_drd_factory(board const& b) -> model_factory
{
  return usb_device_factory<usb_drd>(b);
}
}
