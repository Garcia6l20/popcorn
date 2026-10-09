#pragma once

#include <popcorn/periph/stm32/usb.hpp>
#include <popcorn/periph/usb_cdc_acm.hpp>
#include <popcorn/periph/usb_hid.hpp>
#include <popcorn/periph/usb_host.hpp>
#include <popcorn/periph/usb_uac2.hpp>

#include "access_op.hpp"
#include "alive.hpp"
#include "gil_locks.hpp"
#include "pty.hpp"

#include <reflex/py.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace popcorn::bind
{
namespace py = reflex::py;

namespace detail
{
/** @brief The class driver channels of one virtual USB host, shared by its factories and the
 * links. */
struct usb_hubs
{
  /** @brief CDC ACM channels. */
  std::shared_ptr<periph::usb::cdc_hub> cdc{std::make_shared<periph::usb::cdc_hub>()};
  /** @brief HID channels. */
  std::shared_ptr<periph::usb::hid_hub> hid{std::make_shared<periph::usb::hid_hub>()};
  /** @brief USB Audio 2.0 channels. */
  std::shared_ptr<periph::usb::uac2_hub> uac2{std::make_shared<periph::usb::uac2_hub>()};
};
}

/** @brief Options of the virtual USB host a machine wires to a USB device controller. */
struct[[= py::rename{"UsbHostConfig"}]] usb_host_config
{
  /** @brief Whether the cable is plugged at power-on. */
  bool attached{true};
  /** @brief Connect debounce before the attach reset, in nanoseconds. */
  std::uint64_t debounce_ns{100'000'000};
  /** @brief SE0 time of a bus reset, in nanoseconds, above 0. */
  std::uint64_t reset_ns{10'000'000};
  /** @brief Time a control transfer may take after its SETUP, in nanoseconds, above 0. */
  std::uint64_t timeout_ns{5'000'000'000};
  /** @brief DTR in the SET_CONTROL_LINE_STATE the CDC ACM driver sends at configuration. */
  bool dtr{true};
  /** @brief RTS in the SET_CONTROL_LINE_STATE the CDC ACM driver sends at configuration. */
  bool rts{true};
};

/** @brief An endpoint of an interface, as its descriptor gives it. */
struct[[= py::rename{"UsbEndpointRecord"}]] usb_endpoint_record
{
  /** @brief bEndpointAddress: number in bits 3:0, IN when bit 7 is set. */
  [[= py::readonly]] std::uint8_t address{};
  /** @brief Transfer type, bits 1:0 of bmAttributes: 0 control, 1 iso, 2 bulk, 3 interrupt. */
  [[= py::readonly]] std::uint8_t type{};
  /** @brief bmAttributes. */
  [[= py::readonly]] std::uint8_t attributes{};
  /** @brief wMaxPacketSize, bits 10:0. */
  [[= py::readonly]] std::uint16_t max_packet{};
  /** @brief bInterval. */
  [[= py::readonly]] std::uint8_t interval{};
};

/** @brief One alternate setting of an interface of the configuration the device runs. */
struct[[= py::rename{"UsbInterfaceRecord"}]] usb_interface_record
{
  /** @brief bInterfaceNumber. */
  [[= py::readonly]] std::uint8_t number{};
  /** @brief bAlternateSetting. */
  [[= py::readonly]] std::uint8_t alternate{};
  /** @brief bInterfaceClass. */
  [[= py::readonly]] std::uint8_t class_code{};
  /** @brief bInterfaceSubClass. */
  [[= py::readonly]] std::uint8_t subclass{};
  /** @brief bInterfaceProtocol. */
  [[= py::readonly]] std::uint8_t protocol{};
  /** @brief iInterface. */
  [[= py::readonly]] std::uint8_t string_index{};
  /** @brief Endpoints, in descriptor order. */
  [[= py::readonly]] std::vector<usb_endpoint_record> endpoints{};
};

/** @brief A string descriptor the host read during enumeration, raw. */
struct[[= py::rename{"UsbStringRecord"}]] usb_string_record
{
  /** @brief String index. */
  [[= py::readonly]] std::uint8_t index{};
  /** @brief The descriptor, bLength and bDescriptorType included. */
  [[= py::readonly]] std::vector<std::uint8_t> bytes{};
};

/** @brief How a control transfer submitted through a UsbLink ended. */
struct[[= py::rename{"UsbControlResult"}]] usb_control_result
{
  /** @brief Handle control() returned. */
  [[= py::readonly]] std::uint32_t id{};
  /** @brief How it ended. */
  [[= py::readonly]] periph::usb::transfer_status status{};
  /** @brief Data stage received for IN, sent for OUT. */
  [[= py::readonly]] std::vector<std::uint8_t> data{};
  /** @brief Virtual time of its last transaction, in picoseconds. */
  [[= py::readonly]] std::uint64_t at_ps{};
};

/** @brief A control request the host sent, from its request log. */
struct[[= py::rename{"UsbRequestRecord"}]] usb_request_record
{
  /** @brief Virtual time of its SETUP, in picoseconds. */
  [[= py::readonly]] std::uint64_t at_ps{};
  /** @brief Device address. */
  [[= py::readonly]] std::uint8_t address{};
  /** @brief bmRequestType. */
  [[= py::readonly]] std::uint8_t request_type{};
  /** @brief bRequest. */
  [[= py::readonly]] std::uint8_t request{};
  /** @brief wValue. */
  [[= py::readonly]] std::uint16_t value{};
  /** @brief wIndex. */
  [[= py::readonly]] std::uint16_t index{};
  /** @brief wLength. */
  [[= py::readonly]] std::uint16_t length{};
  /** @brief How it ended, empty while it runs. */
  [[= py::readonly]] std::optional<periph::usb::transfer_status> status{};
};

/** @brief One packet a CDC ACM function sent, or the part of it not read yet. */
struct[[= py::rename{"CdcPacketRecord"}]] cdc_packet_record
{
  /** @brief Its bytes. */
  [[= py::readonly]] std::vector<std::uint8_t> data{};
  /** @brief Virtual time of the transaction that carried it, in picoseconds. */
  [[= py::readonly]] std::uint64_t at_ps{};
};

/** @brief A CDC line coding, PSTN 1.2 table 17. */
struct[[= py::rename{"CdcLineCodingRecord"}]] cdc_line_coding_record
{
  /** @brief dwDTERate. */
  [[= py::readonly]] std::uint32_t baud{};
  /** @brief bCharFormat: 0 one stop bit, 1 one and a half, 2 two. */
  [[= py::readonly]] std::uint8_t stop_bits{};
  /** @brief bParityType: 0 none, 1 odd, 2 even, 3 mark, 4 space. */
  [[= py::readonly]] std::uint8_t parity{};
  /** @brief bDataBits. */
  [[= py::readonly]] std::uint8_t data_bits{};
};

/**
 * @brief One CDC ACM function of a virtual USB host connected to Python, by function index: the
 *        n-th ACM function in interface order. Its queues outlive the drivers the host binds, so
 *        the link works before the device is configured and across an unplug. Valid while the
 *        machine lives, every method raises ConfigError after.
 */
class[[= py::rename{"CdcLink"}]] cdc_link
{
public:
  /**
   * @brief Connects to the channel of one function.
   * @param port The controller.
   * @param hub The channels of the controller's host.
   * @param function Function index.
   * @param alive Cleared when the machine is destroyed.
   * @param host The machine's host, through which requests reach the USB host.
   */
  [[= py::skip]] cdc_link(
      periph::stm32::usb_host_port&         port,
      std::shared_ptr<periph::usb::cdc_hub> hub,
      std::size_t                           function,
      detail::alive_flag                    alive,
      detail::host_ref                      host);

  /**
   * @brief Queues bytes for the device. The driver sends them from the next frame once bound.
   * @param data Bytes.
   */
  [[= py::doc{"Queue bytes for the device."}]] void send(reflex::py::nb::bytes const& data);

  /** @brief Bytes sent and not yet taken by the device, the transfer in progress included. */
  [[= py::doc{"Bytes sent and not yet taken by the device."}]] [[nodiscard]] auto queued() const
      -> std::uint64_t;

  /** @brief Bytes the device sent and not read yet. */
  [[= py::doc{"Bytes the device sent and not read yet."}]] [[nodiscard]] auto available() const
      -> std::uint64_t;

  /** @brief Packets the device sent and not read yet, one partly read included. */
  [[= py::doc{"Packets the device sent and not read yet."}]] [[nodiscard]] auto packets() const
      -> std::uint64_t;

  /**
   * @brief Copies bytes the device sent without taking them, under the link lock.
   * @param start Bytes not read yet skipped.
   * @param count Most bytes copied.
   */
  [[= py::doc{"At most count bytes the device sent from start, left queued."}]] [[nodiscard]] auto
      peek(std::uint64_t start, std::uint64_t count) const -> reflex::py::nb::bytes;

  /**
   * @brief Takes the oldest bytes the device sent.
   * @param count Most bytes taken.
   */
  [[= py::doc{"Take at most count of the oldest bytes the device sent."}]] auto
      drain(std::uint64_t count) -> reflex::py::nb::bytes;

  /**
   * @brief Takes the oldest packets the device sent.
   * @param count Most packets taken.
   */
  [[= py::doc{"Take at most count of the oldest packets the device sent."}]] auto
      drain_packets(std::uint64_t count) -> std::vector<cdc_packet_record>;

  /** @brief Received bytes dropped because the queue held its limit. */
  [[= py::doc{"Received bytes dropped because the queue was full."}]] [[nodiscard]] auto
      dropped() const -> std::uint64_t;

  /**
   * @brief Sets the most received bytes the queue keeps, 16 MiB by default.
   * @param limit Bytes, at least 1.
   * @throws config_error when @p limit is 0.
   */
  [[= py::doc{"Set the most received bytes the queue keeps, for tests."}]] void
      set_byte_limit(std::uint64_t limit);

  /** @brief Whether a CDC ACM driver is bound to the function. */
  [[= py::doc{"Whether a CDC ACM driver is bound to the function."}]] [[nodiscard]] auto
      bound() const -> bool;

  /** @brief Whether the bound driver's SET_CONTROL_LINE_STATE of configuration has ended. */
  [[= py::doc{"Whether the bound driver's SET_CONTROL_LINE_STATE of configuration has ended."}]]
      [[nodiscard]] auto ready() const -> bool;

  /**
   * @brief Queues SET_LINE_CODING through the bound driver.
   * @param baud dwDTERate.
   * @param stop_bits bCharFormat.
   * @param parity bParityType.
   * @param data_bits bDataBits.
   * @return The handle its result carries, 0 when no driver is bound.
   */
  [[= py::doc{"Queue SET_LINE_CODING, return its id, 0 when unbound."}]] auto set_line_coding(
      std::uint32_t baud,
      std::uint8_t  stop_bits,
      std::uint8_t  parity,
      std::uint8_t  data_bits) -> std::uint32_t;

  /**
   * @brief Queues SET_CONTROL_LINE_STATE through the bound driver.
   * @param dtr DTR.
   * @param rts RTS.
   * @return The handle its result carries, 0 when no driver is bound.
   */
  [[= py::doc{"Queue SET_CONTROL_LINE_STATE, return its id, 0 when unbound."}]] auto
      set_control_line_state(bool dtr, bool rts) -> std::uint32_t;

  /**
   * @brief Takes the result of a request queued here.
   * @param id Its handle.
   * @return How it ended, empty while it runs.
   */
  [[= py::doc{"Take how a request queued here ended, None while it runs."}]] auto
      take_result(std::uint32_t id) -> std::optional<periph::usb::transfer_status>;

  /**
   * @brief Stops waiting for a request queued here: its result, now or later, is dropped. Does
   *        nothing once the machine is gone, the bookkeeping goes with it.
   * @param id Its handle.
   */
  [[= py::doc{"Stop waiting for a request queued here, drop its result."}]] void
      forget(std::uint32_t id);

  /** @brief The last line coding the device acknowledged. */
  [[= py::doc{"The last line coding the device acknowledged."}]] [[nodiscard]] auto
      line_coding() const -> cdc_line_coding_record;

  /** @brief The last control line state the device acknowledged: DTR bit 0, RTS bit 1. */
  [[= py::doc{"The last control line state the device acknowledged."}]] [[nodiscard]] auto
      line_state() const -> std::uint8_t;

  /** @brief The UART state bitmap of the last SERIAL_STATE notification. */
  [[= py::doc{"The UART state bitmap of the last SERIAL_STATE notification."}]] [[nodiscard]] auto
      serial_state() const -> std::uint16_t;

  /**
   * @brief The function as a pty bridge's link, over the same channel.
   * @throws config_error once the machine is destroyed.
   */
  [[= py::skip]] [[nodiscard]] auto bridged_link() const -> std::unique_ptr<detail::pty_link>;

  /** @brief send() as a command of MachineHandle.submit_ops(). */
  [[= py::doc{"Command of send."}]] [[nodiscard]] auto
      command_send(reflex::py::nb::bytes const& data) const -> access_op;
  /** @brief set_line_coding() as a command of MachineHandle.submit_ops(), its value the id. */
  [[= py::doc{"Command of set_line_coding."}]] [[nodiscard]] auto command_set_line_coding(
      std::uint32_t baud,
      std::uint8_t  stop_bits,
      std::uint8_t  parity,
      std::uint8_t  data_bits) const -> access_op;
  /** @brief set_control_line_state() as a command of MachineHandle.submit_ops(), its value the
   * id. */
  [[= py::doc{"Command of set_control_line_state."}]] [[nodiscard]] auto
      command_set_control_line_state(bool dtr, bool rts) const -> access_op;

  /** @brief A predicate true once available() reaches @p count. */
  [[= py::doc{"Predicate: available() reaches count."}]] [[nodiscard]] auto
      predicate_available(std::uint64_t count) const -> native_predicate;

  /** @brief A predicate true while available() differs from @p mark. */
  [[= py::doc{"Predicate: available() differs from the mark."}]] [[nodiscard]] auto
      predicate_changed(count_mark const& mark) const -> native_predicate;
  /** @brief A predicate true once packets() reaches @p count. */
  [[= py::doc{"Predicate: packets() reaches count."}]] [[nodiscard]] auto
      predicate_packets(std::uint64_t count) const -> native_predicate;
  /** @brief A predicate true while the function is bound and ready. */
  [[= py::doc{"Predicate: bound and ready."}]] [[nodiscard]] auto predicate_ready() const
      -> native_predicate;
  /** @brief A predicate true once the result of request @p id waits, without taking it. */
  [[= py::doc{"Predicate: the result of a request waits."}]] [[nodiscard]] auto
      predicate_ended(std::uint32_t id) const -> native_predicate;

private:
  template <typename Body> auto locked(Body&& body) const;
  auto request(periph::usb::setup_packet setup, std::vector<std::uint8_t> data) -> std::uint32_t;
  auto request_locked(periph::usb::setup_packet setup, std::vector<std::uint8_t> data)
      -> std::uint32_t;
  void send_bytes(std::vector<std::uint8_t> const& bytes) const;
  [[nodiscard]] auto
      request_command(periph::usb::setup_packet setup, std::vector<std::uint8_t> data) const
      -> access_op;

  periph::stm32::usb_host_port*         port_;
  std::shared_ptr<periph::usb::cdc_hub> hub_;
  std::size_t                           function_;
  detail::alive_flag                    alive_;
  detail::host_ref                      host_;
};

/** @brief One input report a HID function sent. */
struct[[= py::rename{"HidReportRecord"}]] hid_report_record
{
  /** @brief Its bytes, the report ID first when the device numbers its reports. */
  [[= py::readonly]] std::vector<std::uint8_t> data{};
  /** @brief Virtual time of the transaction that carried it, in picoseconds. */
  [[= py::readonly]] std::uint64_t at_ps{};
};

/** @brief How a GET_REPORT or SET_REPORT queued through a HidLink ended. */
struct[[= py::rename{"HidReplyRecord"}]] hid_reply_record
{
  /** @brief How it ended. */
  [[= py::readonly]] periph::usb::transfer_status status{};
  /** @brief Data stage received for GET_REPORT, empty for SET_REPORT. */
  [[= py::readonly]] std::vector<std::uint8_t> data{};
};

class hid_write_claim;

/**
 * @brief One HID function of a virtual USB host connected to Python, by function index: the
 *        n-th HID interface in interface order. Its queues outlive the drivers the host binds,
 *        so the link works before the device is configured and across an unplug. Valid while
 *        the machine lives, every method raises ConfigError after.
 */
class[[= py::rename{"HidLink"}]] hid_link
{
public:
  /**
   * @brief Connects to the channel of one function.
   * @param port The controller.
   * @param hub The channels of the controller's host.
   * @param function Function index.
   * @param alive Cleared when the machine is destroyed.
   * @param host The machine's host, through which requests reach the USB host.
   */
  [[= py::skip]] hid_link(
      periph::stm32::usb_host_port&         port,
      std::shared_ptr<periph::usb::hid_hub> hub,
      std::size_t                           function,
      detail::alive_flag                    alive,
      detail::host_ref                      host);

  /**
   * @brief Queues an output report. The driver sends it from the next frame once the report
   *        descriptor is read.
   * @param data The report as hidraw takes it, the report ID first, 0 when unnumbered.
   * @return The handle its result carries.
   * @throws config_error when @p data is shorter than 2 bytes, as hidraw refuses, or carries
   *         more than 0xffff bytes on the bus.
   */
  [[= py::doc{"Queue an output report, return the id its result carries."}]] auto
      write(reflex::py::nb::bytes const& data) -> std::uint64_t;

  /**
   * @brief Takes the result of an output report queued here.
   * @param id Its handle.
   * @return How it ended, empty while it runs.
   */
  [[= py::doc{"Take how an output report queued here ended, None while it runs."}]] auto
      take_written(std::uint64_t id) -> std::optional<periph::usb::transfer_status>;

  /**
   * @brief Takes the result of an output report queued here when it is in, else stops waiting
   *        for it: the report is still sent, and the driver warns of a failure. One link lock
   *        for both, so no result lands unseen between them. Does nothing once the machine is
   *        gone.
   * @param id Its handle.
   * @return How it ended, empty while it runs.
   */
  [[= py::doc{"Take how an output report queued here ended, else stop waiting for it."}]] auto
      release_written(std::uint64_t id) -> std::optional<periph::usb::transfer_status>;

  /** @brief A claim on the output report a command will queue, for hid_write_claim. */
  [[= py::doc{"Claim the output report a command will queue."}]] [[nodiscard]] auto
      write_claim() const -> hid_write_claim;

  /** @brief Output reports queued and not yet taken by the device, the one in progress
   * included. */
  [[= py::doc{"Output reports queued and not yet taken by the device."}]] [[nodiscard]] auto
      queued() const -> std::uint64_t;

  /** @brief Input reports received and not read yet. */
  [[= py::doc{"Input reports received and not read yet."}]] [[nodiscard]] auto available() const
      -> std::uint64_t;

  /**
   * @brief Takes the oldest input reports.
   * @param count Most reports taken.
   */
  [[= py::doc{"Take at most count of the oldest input reports."}]] auto drain(std::uint64_t count)
      -> std::vector<hid_report_record>;

  /** @brief Input reports dropped because the queue held its limit. */
  [[= py::doc{"Input reports dropped because the queue was full."}]] [[nodiscard]] auto
      dropped() const -> std::uint64_t;

  /**
   * @brief Sets the most input reports the queue keeps, 65536 by default.
   * @param limit Reports, at least 1.
   * @throws config_error when @p limit is 0.
   */
  [[= py::doc{"Set the most input reports the queue keeps, for tests."}]] void
      set_report_limit(std::uint64_t limit);

  /** @brief Whether a HID driver is bound to the function. */
  [[= py::doc{"Whether a HID driver is bound to the function."}]] [[nodiscard]] auto bound() const
      -> bool;

  /** @brief Whether the bound driver has read the report descriptor. */
  [[= py::doc{"Whether the bound driver has read the report descriptor."}]] [[nodiscard]] auto
      ready() const -> bool;

  /** @brief The report descriptor the driver read, empty before. */
  [[= py::doc{"The report descriptor the driver read, empty before."}]] [[nodiscard]] auto
      report_descriptor() const -> reflex::py::nb::bytes;

  /**
   * @brief Queues GET_REPORT through the bound driver.
   * @param type Report type, 1 input, 2 output, 3 feature.
   * @param id Report ID, 0 when unnumbered.
   * @param length wLength.
   * @return The handle its result carries, 0 when no driver is bound.
   */
  [[= py::doc{"Queue GET_REPORT, return its id, 0 when unbound."}]] auto
      get_report(std::uint8_t type, std::uint8_t id, std::uint16_t length) -> std::uint32_t;

  /**
   * @brief Queues SET_REPORT through the bound driver.
   * @param type Report type, 1 input, 2 output, 3 feature.
   * @param id Report ID, 0 when unnumbered.
   * @param data The data stage.
   * @return The handle its result carries, 0 when no driver is bound.
   */
  [[= py::doc{"Queue SET_REPORT, return its id, 0 when unbound."}]] auto
      set_report(std::uint8_t type, std::uint8_t id, reflex::py::nb::bytes const& data)
          -> std::uint32_t;

  /**
   * @brief Takes the result of a request queued here.
   * @param id Its handle.
   * @return How it ended and its data, empty while it runs.
   */
  [[= py::doc{"Take how a request queued here ended, None while it runs."}]] auto
      take_result(std::uint32_t id) -> std::optional<hid_reply_record>;

  /**
   * @brief Stops waiting for a request queued here: its result, now or later, is dropped. Does
   *        nothing once the machine is gone, the bookkeeping goes with it.
   * @param id Its handle.
   */
  [[= py::doc{"Stop waiting for a request queued here, drop its result."}]] void
      forget(std::uint32_t id);

  /** @brief write() as a command of MachineHandle.submit_ops(), its value the id, checked
   * now. */
  [[= py::doc{"Command of write."}]] [[nodiscard]] auto
      command_write(reflex::py::nb::bytes const& data) const -> access_op;
  /** @brief get_report() as a command of MachineHandle.submit_ops(), its value the id. */
  [[= py::doc{"Command of get_report."}]] [[nodiscard]] auto
      command_get_report(std::uint8_t type, std::uint8_t id, std::uint16_t length) const
      -> access_op;
  /** @brief set_report() as a command of MachineHandle.submit_ops(), its value the id. */
  [[= py::doc{"Command of set_report."}]] [[nodiscard]] auto command_set_report(
      std::uint8_t                 type,
      std::uint8_t                 id,
      reflex::py::nb::bytes const& data) const -> access_op;

  /** @brief A predicate true once available() reaches @p count. */
  [[= py::doc{"Predicate: available() reaches count."}]] [[nodiscard]] auto
      predicate_available(std::uint64_t count) const -> native_predicate;
  /** @brief A predicate true while the interface is bound and ready. */
  [[= py::doc{"Predicate: bound and ready."}]] [[nodiscard]] auto predicate_ready() const
      -> native_predicate;
  /** @brief A predicate true once the reply of request @p id waits, without taking it. */
  [[= py::doc{"Predicate: the reply of a request waits."}]] [[nodiscard]] auto
      predicate_ended(std::uint32_t id) const -> native_predicate;
  /** @brief A predicate true once the result of output report @p id waits, without taking it. */
  [[= py::doc{"Predicate: the result of an output report waits."}]] [[nodiscard]] auto
      predicate_written(std::uint64_t id) const -> native_predicate;

private:
  friend class hid_write_claim;

  template <typename Body> auto locked(Body&& body) const;
  auto request(periph::usb::setup_packet setup, std::vector<std::uint8_t> data) -> std::uint32_t;
  auto request_locked(periph::usb::setup_packet setup, std::vector<std::uint8_t> data)
      -> std::uint32_t;
  [[nodiscard]] auto write_bytes(std::vector<std::uint8_t> const& bytes) const -> std::uint64_t;
  [[nodiscard]] auto
      request_command(periph::usb::setup_packet setup, std::vector<std::uint8_t> data) const
      -> access_op;

  periph::stm32::usb_host_port*         port_;
  std::shared_ptr<periph::usb::hid_hub> hub_;
  std::size_t                           function_;
  detail::alive_flag                    alive_;
  detail::host_ref                      host_;
};

/**
 * @brief The output report one command queues, for a caller that may give it up before or
 *        after the command ran. The command and release() meet under the hub's link lock, so
 *        giving it up needs no continuation on the machine's thread. Holds no machine.
 */
class[[= py::rename{"HidWriteClaim"}]] hid_write_claim
{
public:
  /**
   * @brief Claims the next report a command of @p link queues.
   * @param link The function, its host reference dropped.
   */
  [[= py::skip]] explicit hid_write_claim(hid_link link);

  /**
   * @brief A command that queues an output report as HidLink.command_write, awaited until
   *        release().
   * @param data The report as hidraw takes it, the report ID first, 0 when unnumbered.
   * @throws config_error as HidLink.write.
   */
  [[= py::doc{"A command queueing an output report, awaited until release()."}]] [[nodiscard]] auto
      command_write(reflex::py::nb::bytes const& data) const -> access_op;

  /**
   * @brief Gives the report up. Before the command ran, the command queues it unawaited. After,
   *        as HidLink.release_written. Either way the driver warns of a failure not taken here.
   *        Once only, and nothing once the machine is gone.
   * @return How it ended when it was in, empty otherwise.
   */
  [[= py::doc{"Give the report up: its result if in, else the driver warns of a failure."}]] auto
      release() -> std::optional<periph::usb::transfer_status>;

private:
  struct claim_state
  {
    std::uint64_t id{};
    bool          released{};
  };

  hid_link                     link_;
  std::shared_ptr<claim_state> state_;
};

/** @brief One audio streaming direction of an audio function, as the driver found it. */
struct[[= py::rename{"UacStreamRecord"}]] uac_stream_record
{
  /** @brief Whether the bound driver found a streaming interface in this direction. */
  [[= py::readonly]] bool present{};
  /** @brief bInterfaceNumber of the audio streaming interface. */
  [[= py::readonly]] std::uint8_t interface{};
  /** @brief Alternate setting start() selects. */
  [[= py::readonly]] std::uint8_t alternate{};
  /** @brief bEndpointAddress of its isochronous endpoint. */
  [[= py::readonly]] std::uint8_t endpoint{};
  /** @brief wMaxPacketSize of that endpoint. */
  [[= py::readonly]] std::uint16_t max_packet{};
  /** @brief bNrChannels. */
  [[= py::readonly]] std::uint8_t channels{};
  /** @brief bSubslotSize, bytes per sample. */
  [[= py::readonly]] std::uint8_t subslot{};
  /** @brief bBitResolution. */
  [[= py::readonly]] std::uint8_t bits{};
  /** @brief bClockID of its clock source. */
  [[= py::readonly]] std::uint8_t clock{};
  /** @brief Sample rates of the clock's GET_RANGE, in Hz. */
  [[= py::readonly]] std::vector<std::uint32_t> rates{};
  /** @brief The rate it runs at, in Hz, 0 before the clock is read. */
  [[= py::readonly]] std::uint32_t rate{};
  /** @brief Whether its alternate setting is selected and an isochronous packet goes every
   * frame. */
  [[= py::readonly]] bool running{};
};

/** @brief A feature unit of an audio function and what the driver last read or wrote. */
struct[[= py::rename{"UacUnitRecord"}]] uac_unit_record
{
  /** @brief bUnitID. */
  [[= py::readonly]] std::uint8_t id{};
  /** @brief Logical channels, the master channel excluded. */
  [[= py::readonly]] std::uint8_t channels{};
  /** @brief bmaControls, master first. */
  [[= py::readonly]] std::vector<std::uint32_t> controls{};
  /** @brief Mute per channel, master first, None where not read. */
  [[= py::readonly]] std::vector<std::optional<bool>> mute{};
  /** @brief Volume per channel in 1/256 dB, master first, None where not read. */
  [[= py::readonly]] std::vector<std::optional<std::int32_t>> volume{};
  /** @brief Volume minimum per channel, None where not read. */
  [[= py::readonly]] std::vector<std::optional<std::int32_t>> volume_min{};
  /** @brief Volume maximum per channel, None where not read. */
  [[= py::readonly]] std::vector<std::optional<std::int32_t>> volume_max{};
  /** @brief Volume resolution per channel, None where not read. */
  [[= py::readonly]] std::vector<std::optional<std::int32_t>> volume_res{};
};

/** @brief One frame a microphone stream received. */
struct[[= py::rename{"UacFrameRecord"}]] uac_frame_record
{
  /** @brief Samples, interleaved, empty for a gap. */
  [[= py::readonly]] std::vector<std::int32_t> samples{};
  /** @brief Virtual time of its transaction, in picoseconds. */
  [[= py::readonly]] std::uint64_t at_ps{};
};

/**
 * @brief One USB Audio 2.0 function of a virtual USB host connected to Python, by function
 *        index: the n-th audio control interface in interface order. Its queues outlive the
 *        drivers the host binds, so the link works before the device is configured and across an
 *        unplug. Valid while the machine lives, every method raises ConfigError after.
 */
class[[= py::rename{"UacLink"}]] uac_link
{
public:
  /**
   * @brief Connects to the channel of one function.
   * @param port The controller.
   * @param hub The channels of the controller's host.
   * @param function Function index.
   * @param alive Cleared when the machine is destroyed.
   * @param host The machine's host, through which start, stop and features reach the USB host.
   */
  [[= py::skip]] uac_link(
      periph::stm32::usb_host_port&          port,
      std::shared_ptr<periph::usb::uac2_hub> hub,
      std::size_t                            function,
      detail::alive_flag                     alive,
      detail::host_ref                       host);

  /** @brief Whether a USB Audio 2.0 driver is bound to the function. */
  [[= py::doc{"Whether a USB Audio 2.0 driver is bound to the function."}]] [[nodiscard]] auto
      bound() const -> bool;

  /** @brief Whether the bound driver has ended its probe requests. */
  [[= py::doc{"Whether the bound driver has ended its probe requests."}]] [[nodiscard]] auto
      ready() const -> bool;

  /**
   * @brief One streaming direction.
   * @param mic The microphone, else the speaker.
   */
  [[= py::doc{"The microphone stream when mic, else the speaker stream."}]] [[nodiscard]] auto
      stream(bool mic) const -> uac_stream_record;

  /** @brief Feature units, in the order the probe reached them. */
  [[= py::doc{"Feature units, in the order the probe reached them."}]] [[nodiscard]] auto
      units() const -> std::vector<uac_unit_record>;

  /**
   * @brief Queues samples for the speaker, interleaved. They go out once the speaker runs.
   * @param samples Samples, each sent as the low bytes of its two's complement.
   */
  [[= py::doc{"Queue interleaved samples for the speaker."}]] void
      write(std::vector<std::int32_t> const& samples);

  /** @brief Speaker samples written and not yet put in a packet. */
  [[= py::doc{"Speaker samples written and not yet put in a packet."}]] [[nodiscard]] auto
      queued() const -> std::uint64_t;

  /** @brief Whether a speaker packet carrying written samples waits to go on the bus. */
  [[= py::doc{"Whether a speaker packet carrying written samples waits to go on the bus."}]]
      [[nodiscard]] auto sending() const -> bool;

  /** @brief Time of the last speaker packet that carried written samples, in picoseconds. */
  [[= py::doc{
      "Time of the last speaker packet that carried written samples, in ps."}]] [[nodiscard]] auto
      sent_at() const -> std::optional<std::uint64_t>;

  /** @brief Stops or unbinds that dropped written speaker samples, queued or in a packet not
   * sent, since the machine was built or the last restore. */
  [[= py::doc{"Stops or unbinds that dropped written speaker samples."}]] [[nodiscard]] auto
      flushes() const -> std::uint64_t;

  /** @brief Microphone frames received and not read yet. */
  [[= py::doc{"Microphone frames received and not read yet."}]] [[nodiscard]] auto available() const
      -> std::uint64_t;

  /**
   * @brief Takes the oldest microphone frames.
   * @param count Most frames taken.
   */
  [[= py::doc{"Take at most count of the oldest microphone frames."}]] auto
      drain(std::uint64_t count) -> std::vector<uac_frame_record>;

  /** @brief Microphone frames dropped because the queue held its limit. */
  [[= py::doc{"Microphone frames dropped because the queue was full."}]] [[nodiscard]] auto
      dropped() const -> std::uint64_t;

  /** @brief Drops the microphone frames not read yet. */
  [[= py::doc{"Drop the microphone frames not read yet."}]] void clear();

  /**
   * @brief Sets the most microphone frames the queue keeps, 65536 by default.
   * @param limit Frames, at least 1.
   * @throws config_error when @p limit is 0.
   */
  [[= py::doc{"Set the most microphone frames the queue keeps, for tests."}]] void
      set_frame_limit(std::uint64_t limit);

  /**
   * @brief Starts a stream through the bound driver.
   * @param mic The microphone, else the speaker.
   * @return The handle its result carries, 0 when no driver is bound, the stream is missing,
   *         has no rate, runs or is starting.
   */
  [[= py::doc{"Start a stream, return the id its result carries, 0 when refused."}]] auto
      start(bool mic) -> std::uint64_t;

  /**
   * @brief Stops a stream at once and queues SET_INTERFACE 0.
   * @param mic The microphone, else the speaker.
   */
  [[= py::doc{"Stop a stream at once and queue SET_INTERFACE 0."}]] void stop(bool mic);

  /**
   * @brief Queues SET_CUR of a feature unit control through the bound driver.
   * @param unit bUnitID.
   * @param channel 0 for the master channel.
   * @param control 1 mute, 2 volume.
   * @param value The value.
   * @return The handle its result carries, 0 when no driver is bound or the control is not
   *         writable on that channel.
   */
  [[= py::doc{"Queue SET_CUR of a feature unit control, return its id, 0 when refused."}]] auto
      set_feature(std::uint8_t unit, std::uint8_t channel, std::uint8_t control, std::int32_t value)
          -> std::uint64_t;

  /**
   * @brief Takes the result of an operation queued here.
   * @param id Its handle.
   * @return How it ended, empty while it runs.
   */
  [[= py::doc{"Take how an operation queued here ended, None while it runs."}]] auto
      take_result(std::uint64_t id) -> std::optional<periph::usb::transfer_status>;

  /**
   * @brief Stops waiting for an operation queued here: its result, now or later, is dropped.
   *        Does nothing once the machine is gone, the bookkeeping goes with it.
   * @param id Its handle.
   */
  [[= py::doc{"Stop waiting for an operation queued here, drop its result."}]] void
      forget(std::uint64_t id);

  /** @brief start() as a command of MachineHandle.submit_ops(), its value the id. */
  [[= py::doc{"Command of start."}]] [[nodiscard]] auto command_start(bool mic) const -> access_op;
  /** @brief stop() as a command of MachineHandle.submit_ops(). */
  [[= py::doc{"Command of stop."}]] [[nodiscard]] auto command_stop(bool mic) const -> access_op;
  /** @brief write() as a command of MachineHandle.submit_ops(). */
  [[= py::doc{"Command of write."}]] [[nodiscard]] auto
      command_write(std::vector<std::int32_t> const& samples) const -> access_op;
  /** @brief set_feature() as a command of MachineHandle.submit_ops(), its value the id. */
  [[= py::doc{"Command of set_feature."}]] [[nodiscard]] auto command_set_feature(
      std::uint8_t unit,
      std::uint8_t channel,
      std::uint8_t control,
      std::int32_t value) const -> access_op;

  /** @brief A predicate true once available() reaches @p count. */
  [[= py::doc{"Predicate: available() reaches count."}]] [[nodiscard]] auto
      predicate_available(std::uint64_t count) const -> native_predicate;
  /** @brief A predicate true while the function is bound and ready. */
  [[= py::doc{"Predicate: bound and ready."}]] [[nodiscard]] auto predicate_ready() const
      -> native_predicate;
  /** @brief A predicate true once the result of operation @p id waits, without taking it. */
  [[= py::doc{"Predicate: the result of an operation waits."}]] [[nodiscard]] auto
      predicate_ended(std::uint64_t id) const -> native_predicate;
  /**
   * @brief A predicate true once the speaker sent every written sample, nothing queued and no
   *        packet in flight, or once a stop changed flushes() from @p flushes.
   */
  [[= py::doc{
      "Predicate: the speaker drained, or flushes() moved from a count."}]] [[nodiscard]] auto
      predicate_drained(std::uint64_t flushes) const -> native_predicate;

private:
  template <typename Body> auto locked(Body&& body) const;
  template <typename Body> auto engine(Body&& body) const;
  [[nodiscard]] auto            host() const -> periph::usb::host*;
  void                          require_alive() const;
  [[nodiscard]] auto            start_now(bool mic) const -> std::uint64_t;
  void                          stop_now(bool mic) const;
  void                          write_samples(std::vector<std::int32_t> const& samples) const;
  [[nodiscard]] auto            set_feature_now(
      std::uint8_t unit,
      std::uint8_t channel,
      std::uint8_t control,
      std::int32_t value) const -> std::uint64_t;

  periph::stm32::usb_host_port*          port_;
  std::shared_ptr<periph::usb::uac2_hub> hub_;
  std::size_t                            function_;
  detail::alive_flag                     alive_;
  detail::host_ref                       host_;
};

namespace detail
{
/** @brief What a control request knows of its transfer. Only the engine owner touches it. */
struct control_claim
{
  /** @brief The transfer, once the request's command ran. */
  std::optional<periph::usb::transfer_id> id{};
  /** @brief The host's epoch when the command ran: a load since makes the id another's. */
  std::uint64_t epoch = 0;
  /** @brief Whether the request gave its result up. */
  bool forgotten = false;
  /** @brief Whether the request took its result. */
  bool taken = false;
};
}

/**
 * @brief A control request's claim on its result, made before the request is posted, so a
 *        cancel at any point can give the result up.
 */
class[[= py::rename{"UsbControlTicket"}]] usb_control_ticket
{
public:
  /** @brief A fresh claim. */
  [[= py::skip]] usb_control_ticket();

  /** @brief The claim, shared by the request's command, take and forget. */
  [[= py::skip]] [[nodiscard]] auto claim() const noexcept
      -> std::shared_ptr<detail::control_claim> const&;

private:
  std::shared_ptr<detail::control_claim> claim_;
};

/**
 * @brief The virtual USB host of a device controller connected to Python. The host and its
 *        cable live in the controller model and its snapshots: this link only reads them and
 *        queues control transfers. Valid while the machine lives, every method raises
 *        ConfigError after.
 */
class[[= py::rename{"UsbLink"}]] usb_link
{
public:
  /**
   * @brief Connects to the board host of a controller.
   * @param port The controller.
   * @param hubs The channels of the host's class drivers.
   * @param alive Cleared when the machine is destroyed.
   * @param host The machine's host, through which every method but the link getters reaches the
   *        USB host.
   */
  [[= py::skip]] usb_link(
      periph::stm32::usb_host_port& port,
      detail::usb_hubs              hubs,
      detail::alive_flag            alive,
      detail::host_ref              host);

  /** @brief Whether the cable is plugged. */
  [[= py::doc{"Whether the cable is plugged."}]] [[nodiscard]] auto plugged() const -> bool;

  /**
   * @brief Plugs or unplugs the cable.
   * @param attached Whether the cable is plugged.
   */
  [[= py::doc{"Plug or unplug the cable."}]] void plug(bool attached);

  /** @brief Device state as the host sees it. */
  [[= py::doc{"Device state as the host sees it."}]] [[nodiscard]] auto state() const
      -> periph::usb::device_state;

  /** @brief Address tokens go to, 0 before SET_ADDRESS. */
  [[= py::doc{"Address tokens go to, 0 before SET_ADDRESS."}]] [[nodiscard]] auto address() const
      -> std::uint8_t;

  /** @brief bConfigurationValue set, 0 when not configured. */
  [[= py::doc{"bConfigurationValue set, 0 when not configured."}]] [[nodiscard]] auto
      configuration_value() const -> std::uint8_t;

  /** @brief state() as an op of MachineHandle.submit_ops(), its value the state's integer. */
  [[= py::doc{"Op of state, its integer value."}]] [[nodiscard]] auto op_state() const -> access_op;
  /** @brief address() as an op of MachineHandle.submit_ops(). */
  [[= py::doc{"Op of address."}]] [[nodiscard]] auto op_address() const -> access_op;
  /** @brief configuration_value() as an op of MachineHandle.submit_ops(). */
  [[= py::doc{"Op of configuration_value."}]] [[nodiscard]] auto op_configuration_value() const
      -> access_op;
  /** @brief requests() as an op of MachineHandle.submit_ops(). */
  [[= py::doc{"Op of requests."}]] [[nodiscard]] auto op_requests() const -> access_op;

  /** @brief Name of the enumeration step that failed, empty while none did. */
  [[= py::doc{"Name of the enumeration step that failed, None while none did."}]] [[nodiscard]] auto
      failed_step() const -> std::optional<std::string>;

  /** @brief Device descriptor bytes read so far. */
  [[= py::doc{"Device descriptor bytes read so far."}]] [[nodiscard]] auto device_descriptor() const
      -> reflex::py::nb::bytes;

  /** @brief Configuration descriptor bytes read so far, wTotalLength once complete. */
  [[= py::doc{"Configuration descriptor bytes read so far."}]] [[nodiscard]] auto
      configuration_descriptor() const -> reflex::py::nb::bytes;

  /** @brief String descriptors read, string 0 included, in the order read. */
  [[= py::doc{"String descriptors read, string 0 included, in the order read."}]] [[nodiscard]] auto
      strings() const -> std::vector<usb_string_record>;

  /** @brief Interfaces and alternate settings of the configuration, empty before it is read. */
  [[= py::doc{"Interfaces and alternate settings of the configuration read."}]] [[nodiscard]] auto
      interfaces() const -> std::vector<usb_interface_record>;

  /**
   * @brief Queues a control transfer on the default pipe. It enters the host state at once, so a
   *        snapshot taken before it ends restores it queued.
   * @param request_type bmRequestType.
   * @param request bRequest.
   * @param value wValue.
   * @param index wIndex.
   * @param length wLength.
   * @param data OUT data stage, @p length bytes, empty for IN or no data.
   * @return The handle its result carries.
   * @throws config_error when @p data is neither empty nor @p length bytes, or an IN request
   *         carries data.
   */
  [[= py::doc{"Queue a control transfer, return the id its result carries."}]] auto control(
      std::uint8_t                 request_type,
      std::uint8_t                 request,
      std::uint16_t                value,
      std::uint16_t                index,
      std::uint16_t                length,
      reflex::py::nb::bytes const& data) -> std::uint32_t;

  /** @brief Takes the results of the control transfers queued here, oldest first. */
  [[= py::doc{"Take the results of the control transfers, oldest first."}]] auto take_results()
      -> std::vector<usb_control_result>;

  /** @brief The control requests the host sent since the machine was built, oldest first. */
  [[= py::doc{"The control requests the host sent, oldest first."}]] [[nodiscard]] auto
      requests() const -> std::vector<usb_request_record>;

  /** @brief CDC ACM functions of the configuration read, empty before it is read. */
  [[= py::doc{
      "CDC ACM functions of the configuration read, None before it is read."}]] [[nodiscard]] auto
      cdc_functions() const -> std::optional<std::uint64_t>;

  /**
   * @brief Connects to a CDC ACM function.
   * @param function Function index, the n-th ACM function in interface order.
   */
  [[= py::doc{"Connect to the n-th CDC ACM function."}]] [[nodiscard]] auto
      cdc(std::uint64_t function) const -> cdc_link;

  /** @brief HID functions of the configuration read, empty before it is read. */
  [[= py::doc{
      "HID functions of the configuration read, None before it is read."}]] [[nodiscard]] auto
      hid_functions() const -> std::optional<std::uint64_t>;

  /**
   * @brief Connects to a HID function.
   * @param function Function index, the n-th HID interface in interface order.
   */
  [[= py::doc{"Connect to the n-th HID function."}]] [[nodiscard]] auto
      hid(std::uint64_t function) const -> hid_link;

  /** @brief USB Audio 2.0 functions of the configuration read, empty before it is read. */
  [[= py::doc{"USB Audio 2.0 functions of the configuration read, None before it is read."}]]
      [[nodiscard]] auto audio_functions() const -> std::optional<std::uint64_t>;

  /**
   * @brief Connects to a USB Audio 2.0 function.
   * @param function Function index, the n-th audio control interface in interface order.
   */
  [[= py::doc{"Connect to the n-th USB Audio 2.0 function."}]] [[nodiscard]] auto
      audio(std::uint64_t function) const -> uac_link;

  /** @brief plug() as a command of MachineHandle.submit_ops(). */
  [[= py::doc{"Command of plug."}]] [[nodiscard]] auto command_plug(bool attached) const
      -> access_op;
  /** @brief (state(), failed_step()) as an op of MachineHandle.submit_ops(), the state as its
   * integer. */
  [[= py::doc{"Op of (state, failed_step)."}]] [[nodiscard]] auto op_settled() const -> access_op;
  /** @brief The result of the control transfer of @p ticket alone, None while it runs, as an op
   * of MachineHandle.submit_ops(). */
  [[= py::doc{
      "Op taking the result of one control transfer, None while it runs."}]] [[nodiscard]] auto
      op_take_result(usb_control_ticket const& ticket) const -> access_op;

  /** @brief A claim for command_control(). */
  [[= py::doc{"A claim for command_control."}]] [[nodiscard]] auto ticket() const
      -> usb_control_ticket;

  /**
   * @brief Gives the result of the request of @p ticket up: dropped at once when it waits, when
   *        it lands otherwise, never made when the command did not run yet. Runs as the engine
   *        owner, at once while parked, at the next boundary while running, and neither waits
   *        nor raises.
   */
  [[= py::doc{"Give a control request's result up, without waiting or raising."}]] void
      forget(usb_control_ticket const& ticket) const noexcept;

  /** @brief A predicate true once the device is configured or an enumeration step failed. */
  [[= py::doc{"Predicate: configured, or an enumeration step failed."}]] [[nodiscard]] auto
      predicate_settled() const -> native_predicate;
  /** @brief A predicate true once the result of control transfer @p id waits, without taking it.
   */
  [[= py::doc{"Predicate: the result of a control transfer waits."}]] [[nodiscard]] auto
      predicate_control_ended(std::uint32_t id) const -> native_predicate;

  /** @brief control() as a command of MachineHandle.submit_ops(), its value the id, checked
   * now. */
  [[= py::doc{"Command of control."}]] [[nodiscard]] auto command_control(
      usb_control_ticket const&    ticket,
      std::uint8_t                 request_type,
      std::uint8_t                 request,
      std::uint16_t                value,
      std::uint16_t                index,
      std::uint16_t                length,
      reflex::py::nb::bytes const& data) const -> access_op;

private:
  [[nodiscard]] auto            host() const -> periph::usb::host&;
  [[nodiscard]] auto            port() const -> periph::stm32::usb_host_port&;
  template <typename Body> auto engine(Body&& body) const;

  periph::stm32::usb_host_port* port_;
  detail::usb_hubs              hubs_;
  detail::alive_flag            alive_;
  detail::host_ref              host_;
};
}
