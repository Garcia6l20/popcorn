#pragma once

#include <popcorn/periph/stm32/usb.hpp>
#include <popcorn/periph/usb_host.hpp>

#include <reflex/py.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace popcorn::bind
{
namespace py = reflex::py;

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
   * @param alive Cleared when the machine is destroyed.
   */
  [[= py::skip]] usb_link(periph::stm32::usb_host_port& port, std::shared_ptr<bool const> alive);

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

private:
  [[nodiscard]] auto host() const -> periph::usb::host&;
  [[nodiscard]] auto port() const -> periph::stm32::usb_host_port&;

  periph::stm32::usb_host_port* port_;
  std::shared_ptr<bool const>   alive_;
};
}
