#include "usb.hpp"
#include "machine.hpp"

#include <popcorn/core/errors.hpp>

#include <reflex/enum.hpp>

#include <algorithm>
#include <cstddef>
#include <format>
#include <ranges>
#include <span>
#include <string>
#include <vector>

namespace popcorn::bind
{
namespace
{
namespace usb = periph::usb;

auto endpoint_record(usb::endpoint_info const& endpoint) -> usb_endpoint_record
{
  return {
      .address    = endpoint.address,
      .type       = static_cast<std::uint8_t>(endpoint.type),
      .attributes = endpoint.attributes,
      .max_packet = endpoint.max_packet,
      .interval   = endpoint.interval,
  };
}

auto interface_record(usb::interface_info const& setting) -> usb_interface_record
{
  return {
      .number       = setting.number,
      .alternate    = setting.alternate,
      .class_code   = setting.class_code,
      .subclass     = setting.subclass,
      .protocol     = setting.protocol,
      .string_index = setting.string_index,
      .endpoints    = setting.endpoints
                    | std::views::transform(endpoint_record)
                    | std::ranges::to<std::vector>(),
  };
}

auto bytes_of(std::span<std::uint8_t const> bytes) -> reflex::py::nb::bytes
{
  return reflex::py::nb::bytes(bytes.data(), bytes.size());
}
}

usb_link::usb_link(periph::stm32::usb_host_port& port, std::shared_ptr<bool const> alive)
    : port_{&port}, alive_{std::move(alive)}
{}

auto usb_link::port() const -> periph::stm32::usb_host_port&
{
  if(not *alive_)
  {
    throw config_error("usb", 0, "machine", "a USB host whose machine still exists");
  }
  return *port_;
}

auto usb_link::host() const -> periph::usb::host&
{
  return *port().board_host();
}

auto usb_link::plugged() const -> bool
{
  return port().plugged();
}

void usb_link::plug(bool attached)
{
  port().plug(attached);
}

auto usb_link::state() const -> periph::usb::device_state
{
  return host().state();
}

auto usb_link::address() const -> std::uint8_t
{
  return host().address();
}

auto usb_link::configuration_value() const -> std::uint8_t
{
  return host().configuration_value();
}

auto usb_link::failed_step() const -> std::optional<std::string>
{
  auto const failed = host().failed_step();
  if(not failed)
  {
    return std::nullopt;
  }
  return std::string{reflex::enum_value_name(*failed)};
}

auto usb_link::device_descriptor() const -> reflex::py::nb::bytes
{
  return bytes_of(host().device_bytes());
}

auto usb_link::configuration_descriptor() const -> reflex::py::nb::bytes
{
  return bytes_of(host().configuration_bytes());
}

auto usb_link::strings() const -> std::vector<usb_string_record>
{
  return host().strings()
       | std::views::transform([](usb::string_descriptor const& string) {
           return usb_string_record{
               .index = string.index,
               .bytes = string.bytes
                      | std::views::transform(
                            [](std::byte b) { return std::to_integer<std::uint8_t>(b); })
                      | std::ranges::to<std::vector>(),
           };
         })
       | std::ranges::to<std::vector>();
}

auto usb_link::interfaces() const -> std::vector<usb_interface_record>
{
  auto const* const configuration = host().configuration();
  if(configuration == nullptr)
  {
    return {};
  }
  return configuration->interfaces
       | std::views::transform(interface_record)
       | std::ranges::to<std::vector>();
}

auto usb_link::control(
    std::uint8_t                 request_type,
    std::uint8_t                 request,
    std::uint16_t                value,
    std::uint16_t                index,
    std::uint16_t                length,
    reflex::py::nb::bytes const& data) -> std::uint32_t
{
  auto const in = (request_type & 0x80u) != 0;
  if(data.size() != 0 and (in or data.size() != length))
  {
    throw config_error(
        "usb", 0, "data",
        in ? "no data with an IN request"
           : std::format("an OUT data stage of wLength {} bytes, not {}", length, data.size()));
  }
  auto const* const first = static_cast<std::uint8_t const*>(data.data());
  return host().control(
      {.request_type = request_type,
       .request      = request,
       .value        = value,
       .index        = index,
       .length       = length},
      std::vector<std::uint8_t>(first, first + data.size()));
}

auto usb_link::take_results() -> std::vector<usb_control_result>
{
  return host().take_results()
       | std::views::transform([](usb::transfer_result const& result) {
           return usb_control_result{
               .id     = result.id,
               .status = result.status,
               .data   = result.data,
               .at_ps  = result.at.ps,
           };
         })
       | std::ranges::to<std::vector>();
}

auto machine_handle::usb_names() const -> std::vector<std::string>
{
  std::vector<std::string> names;
  for(auto const& unit : machine_->description().peripherals)
  {
    auto* const port = dynamic_cast<periph::stm32::usb_host_port*>(machine_->model(unit.name));
    if(not unit.alias_of and port != nullptr and port->board_host() != nullptr)
    {
      names.push_back(unit.name);
    }
  }
  return names;
}

auto machine_handle::attach_usb(std::string const& peripheral) -> usb_link
{
  auto* const port = dynamic_cast<periph::stm32::usb_host_port*>(machine_->model(peripheral));
  if(port == nullptr or port->board_host() == nullptr)
  {
    throw model_error(peripheral, "", 0);
  }
  return usb_link{*port, alive_};
}
}
