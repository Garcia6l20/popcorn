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

auto packet_record(usb::cdc_packet const& packet) -> cdc_packet_record
{
  return {.data = packet.data, .at_ps = packet.at.ps};
}
}

cdc_link::cdc_link(
    periph::stm32::usb_host_port&         port,
    std::shared_ptr<periph::usb::cdc_hub> hub,
    std::size_t                           function,
    std::shared_ptr<bool const>           alive)
    : port_{&port}, hub_{std::move(hub)}, function_{function}, alive_{std::move(alive)}
{}

auto cdc_link::channel() const -> periph::usb::cdc_channel&
{
  if(not *alive_)
  {
    throw config_error("usb", 0, "machine", "a USB host whose machine still exists");
  }
  return hub_->channel(function_);
}

void cdc_link::send(reflex::py::nb::bytes const& data)
{
  auto const bytes = std::span{static_cast<std::uint8_t const*>(data.data()), data.size()};
  channel().out.append_range(bytes);
}

auto cdc_link::queued() const -> std::uint64_t
{
  auto const& queues = channel();
  return queues.out.size() + queues.in_flight;
}

auto cdc_link::available() const -> std::uint64_t
{
  return channel().in_bytes;
}

auto cdc_link::packets() const -> std::uint64_t
{
  return channel().packets();
}

auto cdc_link::drain(std::uint64_t count) -> reflex::py::nb::bytes
{
  std::vector<std::uint8_t> bytes;
  for(auto const& packet : channel().take(count))
  {
    bytes.append_range(packet.data);
  }
  return bytes_of(bytes);
}

auto cdc_link::drain_packets(std::uint64_t count) -> std::vector<cdc_packet_record>
{
  return channel().take_packets(count)
       | std::views::transform(packet_record)
       | std::ranges::to<std::vector>();
}

auto cdc_link::dropped() const -> std::uint64_t
{
  return channel().dropped;
}

void cdc_link::set_byte_limit(std::uint64_t limit)
{
  if(limit == 0)
  {
    throw config_error("usb", 0, "limit", "a byte limit of at least 1");
  }
  channel().set_limit(limit);
}

auto cdc_link::bound() const -> bool
{
  return channel().comm_interface.has_value();
}

auto cdc_link::request(periph::usb::setup_packet setup, std::vector<std::uint8_t> data)
    -> std::uint32_t
{
  auto&       queues = channel();
  auto* const host   = port_->board_host();
  if(not queues.comm_interface or host == nullptr)
  {
    return 0;
  }
  auto const comm    = *queues.comm_interface;
  auto const drivers = host->drivers();
  auto const found   = std::ranges::find_if(drivers, [&](usb::bound_driver const& bound) {
    return bound.name
        == usb::cdc_acm_name
       and not bound.interfaces.empty()
       and bound.interfaces.front()
        == comm;
  });
  if(found == drivers.end())
  {
    return 0;
  }
  setup.index   = comm;
  auto const id = host->context(static_cast<std::size_t>(found - drivers.begin()))
                      .control(setup, std::move(data));
  queues.awaited.insert(id);
  return id;
}

auto cdc_link::set_line_coding(
    std::uint32_t baud,
    std::uint8_t  stop_bits,
    std::uint8_t  parity,
    std::uint8_t  data_bits) -> std::uint32_t
{
  return request(
      {.request_type = 0x21, .request = 0x20, .value = 0, .index = 0, .length = 7},
      {static_cast<std::uint8_t>(baud), static_cast<std::uint8_t>(baud >> 8),
       static_cast<std::uint8_t>(baud >> 16), static_cast<std::uint8_t>(baud >> 24), stop_bits,
       parity, data_bits});
}

auto cdc_link::set_control_line_state(bool dtr, bool rts) -> std::uint32_t
{
  auto const lines = static_cast<std::uint16_t>((dtr ? 1u : 0u) | (rts ? 2u : 0u));
  return request(
      {.request_type = 0x21, .request = 0x22, .value = lines, .index = 0, .length = 0}, {});
}

auto cdc_link::take_result(std::uint32_t id) -> std::optional<periph::usb::transfer_status>
{
  auto& results = channel().results;
  auto  found   = results.find(id);
  if(found == results.end())
  {
    return std::nullopt;
  }
  auto const status = found->second;
  results.erase(found);
  return status;
}

void cdc_link::forget(std::uint32_t id)
{
  auto& queues = channel();
  queues.awaited.erase(id);
  queues.results.erase(id);
}

auto cdc_link::line_coding() const -> cdc_line_coding_record
{
  auto const& coding = channel().coding;
  return {
      .baud      = coding.baud,
      .stop_bits = coding.stop_bits,
      .parity    = coding.parity,
      .data_bits = coding.data_bits,
  };
}

auto cdc_link::line_state() const -> std::uint8_t
{
  return channel().line_state;
}

auto cdc_link::serial_state() const -> std::uint16_t
{
  return channel().serial_state;
}

hid_link::hid_link(
    periph::stm32::usb_host_port&         port,
    std::shared_ptr<periph::usb::hid_hub> hub,
    std::size_t                           function,
    std::shared_ptr<bool const>           alive)
    : port_{&port}, hub_{std::move(hub)}, function_{function}, alive_{std::move(alive)}
{}

auto hid_link::channel() const -> periph::usb::hid_channel&
{
  if(not *alive_)
  {
    throw config_error("usb", 0, "machine", "a USB host whose machine still exists");
  }
  return hub_->channel(function_);
}

auto hid_link::write(reflex::py::nb::bytes const& data) -> std::uint64_t
{
  auto&             queues = channel();
  auto const* const bytes  = static_cast<std::uint8_t const*>(data.data());
  auto const        stage  = data.size() == 0 ? 0 : data.size() - (bytes[0] == 0 ? 1 : 0);
  if(data.size() < 2 or stage > 0xffff)
  {
    throw config_error(
        "usb", 0, "report",
        std::format(
            "a report of at least 2 bytes, at most 0xffff on the bus, not {} bytes", data.size()));
  }
  auto const id = queues.write({bytes, bytes + data.size()});
  queues.awaited_writes.insert(id);
  return id;
}

auto hid_link::take_written(std::uint64_t id) -> std::optional<periph::usb::transfer_status>
{
  auto& written = channel().written;
  auto  found   = written.find(id);
  if(found == written.end())
  {
    return std::nullopt;
  }
  auto const status = found->second;
  written.erase(found);
  return status;
}

void hid_link::forget_written(std::uint64_t id)
{
  auto& queues = channel();
  queues.awaited_writes.erase(id);
  queues.written.erase(id);
}

auto hid_link::queued() const -> std::uint64_t
{
  return channel().queued();
}

auto hid_link::available() const -> std::uint64_t
{
  return channel().in.size();
}

auto hid_link::drain(std::uint64_t count) -> std::vector<hid_report_record>
{
  return channel().take(count)
       | std::views::transform([](usb::hid_report const& report) {
           return hid_report_record{.data = report.data, .at_ps = report.at.ps};
         })
       | std::ranges::to<std::vector>();
}

auto hid_link::dropped() const -> std::uint64_t
{
  return channel().dropped;
}

void hid_link::set_report_limit(std::uint64_t limit)
{
  if(limit == 0)
  {
    throw config_error("usb", 0, "limit", "a report limit of at least 1");
  }
  channel().set_limit(limit);
}

auto hid_link::bound() const -> bool
{
  return channel().interface.has_value();
}

auto hid_link::report_descriptor() const -> reflex::py::nb::bytes
{
  return bytes_of(channel().report_descriptor);
}

auto hid_link::request(periph::usb::setup_packet setup, std::vector<std::uint8_t> data)
    -> std::uint32_t
{
  auto&       queues = channel();
  auto* const host   = port_->board_host();
  if(not queues.interface or host == nullptr)
  {
    return 0;
  }
  auto const interface = *queues.interface;
  auto const drivers   = host->drivers();
  auto const found     = std::ranges::find_if(drivers, [&](usb::bound_driver const& bound) {
    return bound.name == usb::hid_name and bound.interfaces == std::vector{interface};
  });
  if(found == drivers.end())
  {
    return 0;
  }
  setup.index   = interface;
  auto const id = host->context(static_cast<std::size_t>(found - drivers.begin()))
                      .control(setup, std::move(data));
  queues.awaited.insert(id);
  return id;
}

auto hid_link::get_report(std::uint8_t type, std::uint8_t id, std::uint16_t length) -> std::uint32_t
{
  return request(
      {.request_type = 0xa1,
       .request      = 0x01,
       .value        = static_cast<std::uint16_t>(type << 8 | id),
       .index        = 0,
       .length       = length},
      {});
}

auto hid_link::set_report(std::uint8_t type, std::uint8_t id, reflex::py::nb::bytes const& data)
    -> std::uint32_t
{
  auto const* const first = static_cast<std::uint8_t const*>(data.data());
  return request(
      {.request_type = 0x21,
       .request      = 0x09,
       .value        = static_cast<std::uint16_t>(type << 8 | id),
       .index        = 0,
       .length       = static_cast<std::uint16_t>(data.size())},
      std::vector<std::uint8_t>(first, first + data.size()));
}

auto hid_link::take_result(std::uint32_t id) -> std::optional<hid_reply_record>
{
  auto& results = channel().results;
  auto  found   = results.find(id);
  if(found == results.end())
  {
    return std::nullopt;
  }
  hid_reply_record record{.status = found->second.status, .data = std::move(found->second.data)};
  results.erase(found);
  return record;
}

void hid_link::forget(std::uint32_t id)
{
  auto& queues = channel();
  queues.awaited.erase(id);
  queues.results.erase(id);
}

usb_link::usb_link(
    periph::stm32::usb_host_port& port,
    detail::usb_hubs              hubs,
    std::shared_ptr<bool const>   alive)
    : port_{&port}, hubs_{std::move(hubs)}, alive_{std::move(alive)}
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

auto usb_link::requests() const -> std::vector<usb_request_record>
{
  return host().log()
       | std::views::filter(
             [](usb::log_entry const& entry) { return entry.kind == usb::log_kind::request; })
       | std::views::transform([](usb::log_entry const& entry) {
           return usb_request_record{
               .at_ps        = entry.at.ps,
               .address      = entry.address,
               .request_type = entry.setup.request_type,
               .request      = entry.setup.request,
               .value        = entry.setup.value,
               .index        = entry.setup.index,
               .length       = entry.setup.length,
               .status       = entry.status,
           };
         })
       | std::ranges::to<std::vector>();
}

auto usb_link::cdc_functions() const -> std::optional<std::uint64_t>
{
  auto const* const configuration = host().configuration();
  if(configuration == nullptr)
  {
    return std::nullopt;
  }
  return usb::cdc_acm_functions(*configuration).size();
}

auto usb_link::cdc(std::uint64_t function) const -> cdc_link
{
  return cdc_link{port(), hubs_.cdc, static_cast<std::size_t>(function), alive_};
}

auto usb_link::hid_functions() const -> std::optional<std::uint64_t>
{
  auto const* const configuration = host().configuration();
  if(configuration == nullptr)
  {
    return std::nullopt;
  }
  return usb::hid_functions(*configuration).size();
}

auto usb_link::hid(std::uint64_t function) const -> hid_link
{
  return hid_link{port(), hubs_.hid, static_cast<std::size_t>(function), alive_};
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
  auto const hubs = hubs_.find(peripheral);
  return usb_link{*port, hubs != hubs_.end() ? hubs->second : detail::usb_hubs{}, alive_};
}
}
