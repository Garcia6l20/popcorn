#pragma once

#include <popcorn/periph/stm32/usb.hpp>
#include <popcorn/periph/usb_cdc_acm.hpp>
#include <popcorn/periph/usb_hid.hpp>
#include <popcorn/periph/usb_host.hpp>

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
   */
  [[= py::skip]] cdc_link(
      periph::stm32::usb_host_port&         port,
      std::shared_ptr<periph::usb::cdc_hub> hub,
      std::size_t                           function,
      std::shared_ptr<bool const>           alive);

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
   * @brief Stops waiting for a request queued here: its result, now or later, is dropped.
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

private:
  [[nodiscard]] auto channel() const -> periph::usb::cdc_channel&;
  auto request(periph::usb::setup_packet setup, std::vector<std::uint8_t> data) -> std::uint32_t;

  periph::stm32::usb_host_port*         port_;
  std::shared_ptr<periph::usb::cdc_hub> hub_;
  std::size_t                           function_;
  std::shared_ptr<bool const>           alive_;
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
   */
  [[= py::skip]] hid_link(
      periph::stm32::usb_host_port&         port,
      std::shared_ptr<periph::usb::hid_hub> hub,
      std::size_t                           function,
      std::shared_ptr<bool const>           alive);

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
   * @brief Stops waiting for an output report queued here: its result is dropped, the report
   *        is still sent.
   * @param id Its handle.
   */
  [[= py::doc{"Stop waiting for an output report queued here, drop its result."}]] void
      forget_written(std::uint64_t id);

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
   * @brief Stops waiting for a request queued here: its result, now or later, is dropped.
   * @param id Its handle.
   */
  [[= py::doc{"Stop waiting for a request queued here, drop its result."}]] void
      forget(std::uint32_t id);

private:
  [[nodiscard]] auto channel() const -> periph::usb::hid_channel&;
  auto request(periph::usb::setup_packet setup, std::vector<std::uint8_t> data) -> std::uint32_t;

  periph::stm32::usb_host_port*         port_;
  std::shared_ptr<periph::usb::hid_hub> hub_;
  std::size_t                           function_;
  std::shared_ptr<bool const>           alive_;
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
   */
  [[= py::skip]] usb_link(
      periph::stm32::usb_host_port& port,
      detail::usb_hubs              hubs,
      std::shared_ptr<bool const>   alive);

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

private:
  [[nodiscard]] auto host() const -> periph::usb::host&;
  [[nodiscard]] auto port() const -> periph::stm32::usb_host_port&;

  periph::stm32::usb_host_port* port_;
  detail::usb_hubs              hubs_;
  std::shared_ptr<bool const>   alive_;
};
}
