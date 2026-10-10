#include "usb.hpp"
#include "gil_locks.hpp"
#include "machine.hpp"

#include <popcorn/core/errors.hpp>

#include <reflex/enum.hpp>

#include <nanobind/stl/optional.h>
#include <nanobind/stl/vector.h>

#include <algorithm>
#include <cstddef>
#include <format>
#include <functional>
#include <limits>
#include <ranges>
#include <span>
#include <string>
#include <utility>
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
    detail::alive_flag                    alive,
    detail::host_ref                      host)
    : port_{&port}, hub_{std::move(hub)}, function_{function}, alive_{std::move(alive)},
      host_{std::move(host)}
{}

template <typename Body> auto cdc_link::locked(Body&& body) const
{
  if(not detail::is_alive(alive_))
  {
    throw config_error("usb", 0, "machine", "a USB host whose machine still exists");
  }
  return detail::link_locked(*hub_, [&](core::link_guard const& held) {
    return std::invoke(std::forward<Body>(body), hub_->channel(held, function_));
  });
}

auto cdc_link::bridged_link() const -> std::unique_ptr<detail::pty_link>
{
  if(not detail::is_alive(alive_))
  {
    throw config_error("usb", 0, "machine", "a USB host whose machine still exists");
  }
  return std::make_unique<detail::cdc_pty_link>(hub_, function_);
}

namespace
{
auto byte_vector(reflex::py::nb::bytes const& data) -> std::vector<std::uint8_t>
{
  auto const* const first = static_cast<std::uint8_t const*>(data.data());
  return {first, first + data.size()};
}

auto line_coding_setup(
    std::uint32_t baud,
    std::uint8_t  stop_bits,
    std::uint8_t  parity,
    std::uint8_t  data_bits) -> std::pair<usb::setup_packet, std::vector<std::uint8_t>>
{
  return {
      {.request_type = 0x21, .request = 0x20, .value = 0, .index = 0, .length = 7},
      {static_cast<std::uint8_t>(baud), static_cast<std::uint8_t>(baud >> 8),
       static_cast<std::uint8_t>(baud >> 16), static_cast<std::uint8_t>(baud >> 24), stop_bits,
       parity, data_bits}
  };
}

auto control_line_setup(bool dtr, bool rts) -> usb::setup_packet
{
  auto const lines = static_cast<std::uint16_t>((dtr ? 1u : 0u) | (rts ? 2u : 0u));
  return {.request_type = 0x21, .request = 0x22, .value = lines, .index = 0, .length = 0};
}

auto report_setup(
    std::uint8_t  request_type,
    std::uint8_t  request,
    std::uint8_t  type,
    std::uint8_t  id,
    std::uint16_t length) -> usb::setup_packet
{
  return {
      .request_type = request_type,
      .request      = request,
      .value        = static_cast<std::uint16_t>(type << 8 | id),
      .index        = 0,
      .length       = length};
}
}

void cdc_link::send(reflex::py::nb::bytes const& data)
{
  send_bytes(byte_vector(data));
}

void cdc_link::send_bytes(std::vector<std::uint8_t> const& bytes) const
{
  locked([&](usb::cdc_channel& queues) { queues.out.append_range(bytes); });
}

auto cdc_link::command_send(reflex::py::nb::bytes const& data) const -> access_op
{
  if(not detail::is_alive(alive_))
  {
    throw config_error("usb", 0, "machine", "a USB host whose machine still exists");
  }
  auto link = *this;
  link.host_.reset();
  return detail::link_op(
      [link = std::move(link), bytes = byte_vector(data)] { link.send_bytes(bytes); });
}

auto cdc_link::request_command(usb::setup_packet setup, std::vector<std::uint8_t> data) const
    -> access_op
{
  if(not detail::is_alive(alive_))
  {
    throw config_error("usb", 0, "machine", "a USB host whose machine still exists");
  }
  auto link = *this;
  link.host_.reset();
  return detail::link_op([link = std::move(link), setup, data = std::move(data)] mutable {
    usb::deferred_warnings const deferred;
    return link.request_locked(setup, data);
  });
}

auto cdc_link::command_set_line_coding(
    std::uint32_t baud,
    std::uint8_t  stop_bits,
    std::uint8_t  parity,
    std::uint8_t  data_bits) const -> access_op
{
  auto [setup, data] = line_coding_setup(baud, stop_bits, parity, data_bits);
  return request_command(setup, std::move(data));
}

auto cdc_link::command_set_control_line_state(bool dtr, bool rts) const -> access_op
{
  return request_command(control_line_setup(dtr, rts), {});
}

auto cdc_link::queued() const -> std::uint64_t
{
  return locked(
      [](usb::cdc_channel const& queues) { return queues.out.size() + queues.in_flight; });
}

auto cdc_link::available() const -> std::uint64_t
{
  return locked([](usb::cdc_channel const& queues) { return queues.in_bytes; });
}

auto cdc_link::packets() const -> std::uint64_t
{
  return locked([](usb::cdc_channel const& queues) { return queues.packets(); });
}

auto cdc_link::peek(std::uint64_t start, std::uint64_t count) const -> reflex::py::nb::bytes
{
  auto const copied =
      locked([&](usb::cdc_channel const& queues) { return queues.peek(start, count); });
  return bytes_of(copied);
}

auto cdc_link::drain(std::uint64_t count) -> reflex::py::nb::bytes
{
  return drain_until(count, std::numeric_limits<std::uint64_t>::max());
}

auto cdc_link::drain_until(std::uint64_t count, std::uint64_t until_ps) -> reflex::py::nb::bytes
{
  auto const bytes = locked([&](usb::cdc_channel& queues) {
    std::vector<std::uint8_t> taken;
    for(auto const& packet : queues.take(count, vtime{until_ps}))
    {
      taken.append_range(packet.data);
    }
    return taken;
  });
  return bytes_of(bytes);
}

auto cdc_link::drain_packets(std::uint64_t count) -> std::vector<cdc_packet_record>
{
  return drain_packets_until(count, std::numeric_limits<std::uint64_t>::max());
}

auto cdc_link::drain_packets_until(std::uint64_t count, std::uint64_t until_ps)
    -> std::vector<cdc_packet_record>
{
  auto const taken =
      locked([&](usb::cdc_channel& queues) { return queues.take_packets(count, vtime{until_ps}); });
  return taken | std::views::transform(packet_record) | std::ranges::to<std::vector>();
}

auto cdc_link::dropped() const -> std::uint64_t
{
  return locked([](usb::cdc_channel const& queues) { return queues.dropped; });
}

void cdc_link::set_byte_limit(std::uint64_t limit)
{
  if(limit == 0)
  {
    throw config_error("usb", 0, "limit", "a byte limit of at least 1");
  }
  locked([&](usb::cdc_channel& queues) { queues.set_limit(limit); });
}

auto cdc_link::bound() const -> bool
{
  return locked([](usb::cdc_channel const& queues) { return queues.comm_interface.has_value(); });
}

auto cdc_link::ready() const -> bool
{
  return locked([](usb::cdc_channel const& queues) { return queues.ready; });
}

auto cdc_link::request(periph::usb::setup_packet setup, std::vector<std::uint8_t> data)
    -> std::uint32_t
{
  if(not detail::is_alive(alive_))
  {
    throw config_error("usb", 0, "machine", "a USB host whose machine still exists");
  }
  return detail::host_access(host_, [&] {
    usb::deferred_warnings const deferred;
    return request_locked(setup, std::move(data));
  });
}

auto cdc_link::request_locked(periph::usb::setup_packet setup, std::vector<std::uint8_t> data)
    -> std::uint32_t
{
  return locked([&](usb::cdc_channel& queues) -> std::uint32_t {
    auto* const host = port_->board_host();
    if(not queues.comm_interface or host == nullptr)
    {
      return 0;
    }
    auto const comm    = *queues.comm_interface;
    auto const drivers = host->drivers();
    auto const found   = std::ranges::find_if(drivers, [&](usb::bound_driver const& bound) {
      return bound.name == usb::cdc_acm_name
         and not bound.interfaces.empty()
         and bound.interfaces.front() == comm;
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
  });
}

auto cdc_link::set_line_coding(
    std::uint32_t baud,
    std::uint8_t  stop_bits,
    std::uint8_t  parity,
    std::uint8_t  data_bits) -> std::uint32_t
{
  auto [setup, data] = line_coding_setup(baud, stop_bits, parity, data_bits);
  return request(setup, std::move(data));
}

auto cdc_link::set_control_line_state(bool dtr, bool rts) -> std::uint32_t
{
  return request(control_line_setup(dtr, rts), {});
}

auto cdc_link::take_result(std::uint32_t id) -> std::optional<periph::usb::transfer_status>
{
  return locked([&](usb::cdc_channel& queues) -> std::optional<usb::transfer_status> {
    auto found = queues.results.find(id);
    if(found == queues.results.end())
    {
      return std::nullopt;
    }
    auto const status = found->second;
    queues.results.erase(found);
    return status;
  });
}

void cdc_link::forget(std::uint32_t id)
{
  if(not detail::is_alive(alive_))
  {
    return;
  }
  try
  {
    locked([&](usb::cdc_channel& queues) {
      queues.awaited.erase(id);
      queues.results.erase(id);
    });
  }
  catch(config_error const&)
  {}
}

auto cdc_link::line_coding() const -> cdc_line_coding_record
{
  return locked([](usb::cdc_channel const& queues) {
    return cdc_line_coding_record{
        .baud      = queues.coding.baud,
        .stop_bits = queues.coding.stop_bits,
        .parity    = queues.coding.parity,
        .data_bits = queues.coding.data_bits,
    };
  });
}

auto cdc_link::line_state() const -> std::uint8_t
{
  return locked([](usb::cdc_channel const& queues) { return queues.line_state; });
}

auto cdc_link::serial_state() const -> std::uint16_t
{
  return locked([](usb::cdc_channel const& queues) { return queues.serial_state; });
}

hid_link::hid_link(
    periph::stm32::usb_host_port&         port,
    std::shared_ptr<periph::usb::hid_hub> hub,
    std::size_t                           function,
    detail::alive_flag                    alive,
    detail::host_ref                      host)
    : port_{&port}, hub_{std::move(hub)}, function_{function}, alive_{std::move(alive)},
      host_{std::move(host)}
{}

template <typename Body> auto hid_link::locked(Body&& body) const
{
  if(not detail::is_alive(alive_))
  {
    throw config_error("usb", 0, "machine", "a USB host whose machine still exists");
  }
  return detail::link_locked(*hub_, [&](core::link_guard const& held) {
    return std::invoke(std::forward<Body>(body), hub_->channel(held, function_));
  });
}

namespace
{
auto checked_report(reflex::py::nb::bytes const& data) -> std::vector<std::uint8_t>
{
  auto       report = byte_vector(data);
  auto const stage  = report.empty() ? 0 : report.size() - (report[0] == 0 ? 1 : 0);
  if(report.size() < 2 or stage > 0xffff)
  {
    throw config_error(
        "usb", 0, "report",
        std::format(
            "a report of at least 2 bytes, at most 0xffff on the bus, not {} bytes",
            report.size()));
  }
  return report;
}
}

auto hid_link::write(reflex::py::nb::bytes const& data) -> std::uint64_t
{
  return write_bytes(checked_report(data));
}

auto hid_link::write_bytes(std::vector<std::uint8_t> const& bytes) const -> std::uint64_t
{
  return locked([&](usb::hid_channel& queues) {
    auto const id = queues.write(bytes);
    queues.awaited_writes.insert(id);
    return id;
  });
}

auto hid_link::command_write(reflex::py::nb::bytes const& data) const -> access_op
{
  if(not detail::is_alive(alive_))
  {
    throw config_error("usb", 0, "machine", "a USB host whose machine still exists");
  }
  auto link = *this;
  link.host_.reset();
  return detail::link_op(
      [link = std::move(link), report = checked_report(data)] { return link.write_bytes(report); });
}

auto hid_link::request_command(usb::setup_packet setup, std::vector<std::uint8_t> data) const
    -> access_op
{
  if(not detail::is_alive(alive_))
  {
    throw config_error("usb", 0, "machine", "a USB host whose machine still exists");
  }
  auto link = *this;
  link.host_.reset();
  return detail::link_op([link = std::move(link), setup, data = std::move(data)] mutable {
    usb::deferred_warnings const deferred;
    return link.request_locked(setup, data);
  });
}

auto hid_link::command_get_report(std::uint8_t type, std::uint8_t id, std::uint16_t length) const
    -> access_op
{
  return request_command(report_setup(0xa1, 0x01, type, id, length), {});
}

auto hid_link::command_set_report(
    std::uint8_t                 type,
    std::uint8_t                 id,
    reflex::py::nb::bytes const& data) const -> access_op
{
  auto       stage  = byte_vector(data);
  auto const length = static_cast<std::uint16_t>(stage.size());
  return request_command(report_setup(0x21, 0x09, type, id, length), std::move(stage));
}

auto hid_link::take_written(std::uint64_t id) -> std::optional<periph::usb::transfer_status>
{
  return locked([&](usb::hid_channel& queues) -> std::optional<usb::transfer_status> {
    auto found = queues.written.find(id);
    if(found == queues.written.end())
    {
      return std::nullopt;
    }
    auto const status = found->second;
    queues.written.erase(found);
    return status;
  });
}

namespace
{
auto taken_or_forgotten(usb::hid_channel& queues, std::uint64_t id)
    -> std::optional<usb::transfer_status>
{
  queues.awaited_writes.erase(id);
  auto const found = queues.written.find(id);
  if(found == queues.written.end())
  {
    return std::nullopt;
  }
  auto const status = found->second;
  queues.written.erase(found);
  return status;
}
}

auto hid_link::release_written(std::uint64_t id) -> std::optional<periph::usb::transfer_status>
{
  if(not detail::is_alive(alive_))
  {
    return std::nullopt;
  }
  try
  {
    return locked([&](usb::hid_channel& queues) { return taken_or_forgotten(queues, id); });
  }
  catch(config_error const&)
  {
    return std::nullopt;
  }
}

auto hid_link::write_claim() const -> hid_write_claim
{
  if(not detail::is_alive(alive_))
  {
    throw config_error("usb", 0, "machine", "a USB host whose machine still exists");
  }
  auto link = *this;
  link.host_.reset();
  return hid_write_claim{std::move(link)};
}

hid_write_claim::hid_write_claim(hid_link link)
    : link_{std::move(link)}, state_{std::make_shared<claim_state>()}
{}

auto hid_write_claim::command_write(reflex::py::nb::bytes const& data) const -> access_op
{
  return detail::link_op([link = link_, claimed = state_, report = checked_report(data)] {
    return link.locked([&](usb::hid_channel& queues) {
      auto const id = queues.write(report);
      if(not claimed->released)
      {
        queues.awaited_writes.insert(id);
      }
      claimed->id = id;
      return id;
    });
  });
}

auto hid_write_claim::release() -> std::optional<periph::usb::transfer_status>
{
  if(not detail::is_alive(link_.alive_))
  {
    return std::nullopt;
  }
  try
  {
    return link_.locked([&](usb::hid_channel& queues) -> std::optional<usb::transfer_status> {
      if(std::exchange(state_->released, true) or state_->id == 0)
      {
        return std::nullopt;
      }
      return taken_or_forgotten(queues, state_->id);
    });
  }
  catch(config_error const&)
  {
    return std::nullopt;
  }
}

auto hid_link::queued() const -> std::uint64_t
{
  return locked([](usb::hid_channel const& queues) { return queues.queued(); });
}

auto hid_link::available() const -> std::uint64_t
{
  return locked([](usb::hid_channel const& queues) -> std::uint64_t { return queues.in.size(); });
}

auto hid_link::drain(std::uint64_t count) -> std::vector<hid_report_record>
{
  auto const taken = locked([&](usb::hid_channel& queues) { return queues.take(count); });
  return taken
       | std::views::transform([](usb::hid_report const& report) {
           return hid_report_record{.data = report.data, .at_ps = report.at.ps};
         })
       | std::ranges::to<std::vector>();
}

auto hid_link::dropped() const -> std::uint64_t
{
  return locked([](usb::hid_channel const& queues) { return queues.dropped; });
}

void hid_link::set_report_limit(std::uint64_t limit)
{
  if(limit == 0)
  {
    throw config_error("usb", 0, "limit", "a report limit of at least 1");
  }
  locked([&](usb::hid_channel& queues) { queues.set_limit(limit); });
}

auto hid_link::bound() const -> bool
{
  return locked([](usb::hid_channel const& queues) { return queues.interface.has_value(); });
}

auto hid_link::ready() const -> bool
{
  return locked([](usb::hid_channel const& queues) { return queues.ready; });
}

auto hid_link::report_descriptor() const -> reflex::py::nb::bytes
{
  auto const descriptor =
      locked([](usb::hid_channel const& queues) { return queues.report_descriptor; });
  return bytes_of(descriptor);
}

auto hid_link::request(periph::usb::setup_packet setup, std::vector<std::uint8_t> data)
    -> std::uint32_t
{
  if(not detail::is_alive(alive_))
  {
    throw config_error("usb", 0, "machine", "a USB host whose machine still exists");
  }
  return detail::host_access(host_, [&] {
    usb::deferred_warnings const deferred;
    return request_locked(setup, std::move(data));
  });
}

auto hid_link::request_locked(periph::usb::setup_packet setup, std::vector<std::uint8_t> data)
    -> std::uint32_t
{
  return locked([&](usb::hid_channel& queues) -> std::uint32_t {
    auto* const host = port_->board_host();
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
  });
}

auto hid_link::get_report(std::uint8_t type, std::uint8_t id, std::uint16_t length) -> std::uint32_t
{
  return request(report_setup(0xa1, 0x01, type, id, length), {});
}

auto hid_link::set_report(std::uint8_t type, std::uint8_t id, reflex::py::nb::bytes const& data)
    -> std::uint32_t
{
  auto       stage  = byte_vector(data);
  auto const length = static_cast<std::uint16_t>(stage.size());
  return request(report_setup(0x21, 0x09, type, id, length), std::move(stage));
}

auto hid_link::take_result(std::uint32_t id) -> std::optional<hid_reply_record>
{
  return locked([&](usb::hid_channel& queues) -> std::optional<hid_reply_record> {
    auto found = queues.results.find(id);
    if(found == queues.results.end())
    {
      return std::nullopt;
    }
    hid_reply_record record{.status = found->second.status, .data = std::move(found->second.data)};
    queues.results.erase(found);
    return record;
  });
}

void hid_link::forget(std::uint32_t id)
{
  if(not detail::is_alive(alive_))
  {
    return;
  }
  try
  {
    locked([&](usb::hid_channel& queues) {
      queues.awaited.erase(id);
      queues.results.erase(id);
    });
  }
  catch(config_error const&)
  {}
}

uac_link::uac_link(
    periph::stm32::usb_host_port&          port,
    std::shared_ptr<periph::usb::uac2_hub> hub,
    std::size_t                            function,
    detail::alive_flag                     alive,
    detail::host_ref                       host)
    : port_{&port}, hub_{std::move(hub)}, function_{function}, alive_{std::move(alive)},
      host_{std::move(host)}
{}

template <typename Body> auto uac_link::locked(Body&& body) const
{
  if(not detail::is_alive(alive_))
  {
    throw config_error("usb", 0, "machine", "a USB host whose machine still exists");
  }
  return detail::link_locked(*hub_, [&](core::link_guard const& held) {
    return std::invoke(std::forward<Body>(body), hub_->channel(held, function_));
  });
}

template <typename Body> auto uac_link::engine(Body&& body) const
{
  if(not detail::is_alive(alive_))
  {
    throw config_error("usb", 0, "machine", "a USB host whose machine still exists");
  }
  return detail::host_access(host_, std::forward<Body>(body));
}

void uac_link::require_alive() const
{
  if(not detail::is_alive(alive_))
  {
    throw config_error("usb", 0, "machine", "a USB host whose machine still exists");
  }
}

auto uac_link::host() const -> periph::usb::host*
{
  if(not detail::is_alive(alive_))
  {
    throw config_error("usb", 0, "machine", "a USB host whose machine still exists");
  }
  return port_->board_host();
}

auto uac_link::bound() const -> bool
{
  return locked([](usb::uac2_channel const& queues) { return queues.interface.has_value(); });
}

auto uac_link::ready() const -> bool
{
  return locked([](usb::uac2_channel const& queues) { return queues.ready; });
}

auto uac_link::stream(bool mic) const -> uac_stream_record
{
  return locked([&](usb::uac2_channel& queues) -> uac_stream_record {
    auto const& found =
        queues.stream(mic ? usb::uac2_direction::mic : usb::uac2_direction::speaker);
    if(not found.interface)
    {
      return {};
    }
    return {
        .present    = true,
        .interface  = *found.interface,
        .alternate  = found.alternate,
        .endpoint   = found.endpoint,
        .max_packet = found.max_packet,
        .channels   = found.format.channels,
        .subslot    = found.format.subslot,
        .bits       = found.format.bits,
        .clock      = found.clock,
        .rates      = found.rates,
        .rate       = found.rate,
        .running    = found.running,
    };
  });
}

auto uac_link::units() const -> std::vector<uac_unit_record>
{
  auto widen = [](std::optional<std::int16_t> value) -> std::optional<std::int32_t> {
    return value ? std::optional<std::int32_t>{*value} : std::nullopt;
  };
  auto field = [](std::optional<usb::uac2_range> const& range,
                  std::int32_t usb::uac2_range::* member) -> std::optional<std::int32_t> {
    return range ? std::optional{(*range).*member} : std::nullopt;
  };
  return locked([&](usb::uac2_channel const& queues) {
    std::vector<uac_unit_record> out;
    for(auto const& unit : queues.units)
    {
      uac_unit_record record{
          .id = unit.id, .channels = unit.channels, .controls = unit.controls, .mute = unit.mute};
      for(std::size_t index = 0; index < unit.volume.size(); ++index)
      {
        record.volume.push_back(widen(unit.volume[index]));
        record.volume_min.push_back(field(unit.volume_range[index], &usb::uac2_range::min));
        record.volume_max.push_back(field(unit.volume_range[index], &usb::uac2_range::max));
        record.volume_res.push_back(field(unit.volume_range[index], &usb::uac2_range::res));
      }
      out.push_back(std::move(record));
    }
    return out;
  });
}

void uac_link::write(std::vector<std::int32_t> const& samples)
{
  write_samples(samples);
}

void uac_link::write_samples(std::vector<std::int32_t> const& samples) const
{
  locked([&](usb::uac2_channel& queues) { queues.speaker.out.append_range(samples); });
}

auto uac_link::command_write(std::vector<std::int32_t> const& samples) const -> access_op
{
  require_alive();
  auto link = *this;
  link.host_.reset();
  return detail::link_op([link = std::move(link), samples] { link.write_samples(samples); });
}

auto uac_link::queued() const -> std::uint64_t
{
  return locked(
      [](usb::uac2_channel const& queues) -> std::uint64_t { return queues.speaker.out.size(); });
}

auto uac_link::sending() const -> bool
{
  return locked([](usb::uac2_channel const& queues) { return queues.speaker.sending; });
}

auto uac_link::sent_at() const -> std::optional<std::uint64_t>
{
  return locked([](usb::uac2_channel const& queues) -> std::optional<std::uint64_t> {
    auto const& at = queues.speaker.sent_at;
    return at ? std::optional{at->ps} : std::nullopt;
  });
}

auto uac_link::flushes() const -> std::uint64_t
{
  return locked([](usb::uac2_channel const& queues) { return queues.speaker.flushes; });
}

auto uac_link::available() const -> std::uint64_t
{
  return locked(
      [](usb::uac2_channel const& queues) -> std::uint64_t { return queues.mic.in.size(); });
}

auto uac_link::drain(std::uint64_t count) -> std::vector<uac_frame_record>
{
  auto taken = locked([&](usb::uac2_channel& queues) { return queues.mic.take(count); });
  return taken
       | std::views::as_rvalue
       | std::views::transform([](usb::uac2_frame&& frame) {
           return uac_frame_record{.samples = std::move(frame.samples), .at_ps = frame.at.ps};
         })
       | std::ranges::to<std::vector>();
}

auto uac_link::dropped() const -> std::uint64_t
{
  return locked([](usb::uac2_channel const& queues) { return queues.mic.dropped; });
}

void uac_link::clear()
{
  locked([](usb::uac2_channel& queues) { queues.mic.in.clear(); });
}

void uac_link::set_frame_limit(std::uint64_t limit)
{
  if(limit == 0)
  {
    throw config_error("usb", 0, "limit", "a frame limit of at least 1");
  }
  locked([&](usb::uac2_channel& queues) { queues.mic.set_limit(limit); });
}

auto uac_link::start(bool mic) -> std::uint64_t
{
  return engine([&] { return start_now(mic); });
}

auto uac_link::start_now(bool mic) const -> std::uint64_t
{
  auto* const owner = host();
  if(owner == nullptr)
  {
    return 0;
  }
  return usb::uac2_start(
      *owner, *hub_, function_, mic ? usb::uac2_direction::mic : usb::uac2_direction::speaker);
}

auto uac_link::command_start(bool mic) const -> access_op
{
  require_alive();
  auto link = *this;
  link.host_.reset();
  return detail::link_op([link = std::move(link), mic] { return link.start_now(mic); });
}

void uac_link::stop(bool mic)
{
  engine([&] { stop_now(mic); });
}

void uac_link::stop_now(bool mic) const
{
  if(auto* const owner = host())
  {
    usb::uac2_stop(
        *owner, *hub_, function_, mic ? usb::uac2_direction::mic : usb::uac2_direction::speaker);
  }
}

auto uac_link::command_stop(bool mic) const -> access_op
{
  require_alive();
  auto link = *this;
  link.host_.reset();
  return detail::link_op([link = std::move(link), mic] { link.stop_now(mic); });
}

auto uac_link::set_feature(
    std::uint8_t unit,
    std::uint8_t channel,
    std::uint8_t control,
    std::int32_t value) -> std::uint64_t
{
  return engine([&] { return set_feature_now(unit, channel, control, value); });
}

auto uac_link::set_feature_now(
    std::uint8_t unit,
    std::uint8_t channel,
    std::uint8_t control,
    std::int32_t value) const -> std::uint64_t
{
  auto* const owner = host();
  if(owner == nullptr
     or (control != std::to_underlying(usb::uac2_control::mute)
         and control != std::to_underlying(usb::uac2_control::volume)))
  {
    return 0;
  }
  return usb::uac2_set_feature(
      *owner, *hub_, function_, unit, channel, static_cast<usb::uac2_control>(control), value);
}

auto uac_link::command_set_feature(
    std::uint8_t unit,
    std::uint8_t channel,
    std::uint8_t control,
    std::int32_t value) const -> access_op
{
  require_alive();
  auto link = *this;
  link.host_.reset();
  return detail::link_op([link = std::move(link), unit, channel, control, value] {
    return link.set_feature_now(unit, channel, control, value);
  });
}

auto uac_link::take_result(std::uint64_t id) -> std::optional<periph::usb::transfer_status>
{
  return locked([&](usb::uac2_channel& queues) -> std::optional<usb::transfer_status> {
    auto found = queues.results.find(id);
    if(found == queues.results.end())
    {
      return std::nullopt;
    }
    auto const status = found->second;
    queues.results.erase(found);
    return status;
  });
}

void uac_link::forget(std::uint64_t id)
{
  if(not detail::is_alive(alive_))
  {
    return;
  }
  try
  {
    locked([&](usb::uac2_channel& queues) {
      queues.awaited.erase(id);
      queues.results.erase(id);
    });
  }
  catch(config_error const&)
  {}
}

usb_link::usb_link(
    periph::stm32::usb_host_port& port,
    detail::usb_hubs              hubs,
    detail::alive_flag            alive,
    detail::host_ref              host)
    : port_{&port}, hubs_{std::move(hubs)}, alive_{std::move(alive)}, host_{std::move(host)}
{}

template <typename Body> auto usb_link::engine(Body&& body) const
{
  std::ignore = port();
  return detail::host_access(host_, std::forward<Body>(body));
}

auto usb_link::port() const -> periph::stm32::usb_host_port&
{
  if(not detail::is_alive(alive_))
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
  return engine([this] { return port().plugged(); });
}

void usb_link::plug(bool attached)
{
  engine([this, attached] { port().plug(attached); });
}

auto usb_link::command_plug(bool attached) const -> access_op
{
  std::ignore = port();
  auto link   = *this;
  link.host_.reset();
  return detail::link_op([link = std::move(link), attached] { link.port().plug(attached); });
}

auto usb_link::state() const -> periph::usb::device_state
{
  return engine([this] { return host().state(); });
}

auto usb_link::address() const -> std::uint8_t
{
  return engine([this] { return host().address(); });
}

auto usb_link::configuration_value() const -> std::uint8_t
{
  return engine([this] { return host().configuration_value(); });
}

auto usb_link::failed_step() const -> std::optional<std::string>
{
  return engine([&]() -> std::optional<std::string> {
    auto const failed = host().failed_step();
    if(not failed)
    {
      return std::nullopt;
    }
    return std::string{reflex::enum_value_name(*failed)};
  });
}

auto usb_link::device_descriptor() const -> reflex::py::nb::bytes
{
  auto const bytes =
      engine([this] { return host().device_bytes() | std::ranges::to<std::vector>(); });
  return bytes_of(bytes);
}

auto usb_link::configuration_descriptor() const -> reflex::py::nb::bytes
{
  auto const bytes =
      engine([this] { return host().configuration_bytes() | std::ranges::to<std::vector>(); });
  return bytes_of(bytes);
}

auto usb_link::strings() const -> std::vector<usb_string_record>
{
  return engine([&]() -> std::vector<usb_string_record> {
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
  });
}

auto usb_link::interfaces() const -> std::vector<usb_interface_record>
{
  return engine([&]() -> std::vector<usb_interface_record> {
    auto const* const configuration = host().configuration();
    if(configuration == nullptr)
    {
      return {};
    }
    return configuration->interfaces
         | std::views::transform(interface_record)
         | std::ranges::to<std::vector>();
  });
}

namespace
{
auto control_stage(
    std::uint8_t                 request_type,
    std::uint16_t                length,
    reflex::py::nb::bytes const& data) -> std::vector<std::uint8_t>
{
  auto const in = (request_type & 0x80u) != 0;
  if(data.size() != 0 and (in or data.size() != length))
  {
    throw config_error(
        "usb", 0, "data",
        in ? "no data with an IN request"
           : std::format("an OUT data stage of wLength {} bytes, not {}", length, data.size()));
  }
  return byte_vector(data);
}
}

auto usb_link::control(
    std::uint8_t                 request_type,
    std::uint8_t                 request,
    std::uint16_t                value,
    std::uint16_t                index,
    std::uint16_t                length,
    reflex::py::nb::bytes const& data) -> std::uint32_t
{
  auto stage = control_stage(request_type, length, data);
  return engine([&] {
    return host().control(
        {.request_type = request_type,
         .request      = request,
         .value        = value,
         .index        = index,
         .length       = length},
        std::move(stage));
  });
}

auto usb_link::command_control(
    usb_control_ticket const&    ticket,
    std::uint8_t                 request_type,
    std::uint8_t                 request,
    std::uint16_t                value,
    std::uint16_t                index,
    std::uint16_t                length,
    reflex::py::nb::bytes const& data) const -> access_op
{
  std::ignore = port();
  usb::setup_packet const setup{
      .request_type = request_type,
      .request      = request,
      .value        = value,
      .index        = index,
      .length       = length};
  auto link = *this;
  link.host_.reset();
  return detail::link_op([link  = std::move(link), setup,
                          stage = control_stage(request_type, length, data),
                          claim = ticket.claim()] {
    auto& on     = link.host();
    claim->id    = on.control(setup, stage);
    claim->epoch = on.epoch();
    if(claim->forgotten)
    {
      on.forget(*claim->id);
    }
    return *claim->id;
  });
}

usb_control_ticket::usb_control_ticket() : claim_{std::make_shared<detail::control_claim>()}
{}

auto usb_control_ticket::claim() const noexcept -> std::shared_ptr<detail::control_claim> const&
{
  return claim_;
}

auto usb_link::ticket() const -> usb_control_ticket
{
  return {};
}

void usb_link::forget(usb_control_ticket const& ticket) const noexcept
{
  detail::retire(host_, [port = port_, alive = alive_, claim = ticket.claim()] {
    claim->forgotten = true;
    if(not claim->id or claim->taken or not detail::is_alive(alive))
    {
      return;
    }
    auto& on = *port->board_host();
    if(on.epoch() == claim->epoch)
    {
      on.forget(*claim->id);
    }
  });
}

auto usb_link::take_results() -> std::vector<usb_control_result>
{
  return engine([&]() -> std::vector<usb_control_result> {
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
  });
}

namespace
{
auto requests_of(usb::host const& on) -> std::vector<usb_request_record>
{
  return on.log()
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

auto board_host(periph::stm32::usb_host_port* port, detail::alive_flag const& alive) -> usb::host&
{
  if(not detail::is_alive(alive))
  {
    throw config_error("usb", 0, "machine", "a USB host whose machine still exists");
  }
  return *port->board_host();
}
}

auto usb_link::requests() const -> std::vector<usb_request_record>
{
  return engine([&] { return requests_of(host()); });
}

auto usb_link::op_state() const -> access_op
{
  std::ignore = port();
  return detail::link_op(
      [port = port_, alive = alive_] { return board_host(port, alive).state(); });
}

auto usb_link::op_address() const -> access_op
{
  std::ignore = port();
  return detail::link_op(
      [port = port_, alive = alive_] { return board_host(port, alive).address(); });
}

auto usb_link::op_configuration_value() const -> access_op
{
  std::ignore = port();
  return detail::link_op(
      [port = port_, alive = alive_] { return board_host(port, alive).configuration_value(); });
}

auto usb_link::op_requests() const -> access_op
{
  std::ignore = port();
  return detail::link_op([port = port_, alive = alive_] {
    return std::function<reflex::py::nb::object()>{
        [found = requests_of(board_host(port, alive))] { return reflex::py::nb::cast(found); }};
  });
}

auto usb_link::cdc_functions() const -> std::optional<std::uint64_t>
{
  return engine([&]() -> std::optional<std::uint64_t> {
    auto const* const configuration = host().configuration();
    if(configuration == nullptr)
    {
      return std::nullopt;
    }
    return usb::cdc_acm_functions(*configuration).size();
  });
}

auto usb_link::cdc(std::uint64_t function) const -> cdc_link
{
  return cdc_link{port(), hubs_.cdc, static_cast<std::size_t>(function), alive_, host_};
}

auto usb_link::hid_functions() const -> std::optional<std::uint64_t>
{
  return engine([&]() -> std::optional<std::uint64_t> {
    auto const* const configuration = host().configuration();
    if(configuration == nullptr)
    {
      return std::nullopt;
    }
    return usb::hid_functions(*configuration).size();
  });
}

auto usb_link::hid(std::uint64_t function) const -> hid_link
{
  return hid_link{port(), hubs_.hid, static_cast<std::size_t>(function), alive_, host_};
}

auto usb_link::audio_functions() const -> std::optional<std::uint64_t>
{
  return engine([&]() -> std::optional<std::uint64_t> {
    auto const* const configuration = host().configuration();
    if(configuration == nullptr)
    {
      return std::nullopt;
    }
    return usb::uac2_functions(*configuration).size();
  });
}

auto usb_link::audio(std::uint64_t function) const -> uac_link
{
  return uac_link{port(), hubs_.uac2, static_cast<std::size_t>(function), alive_, host_};
}

auto machine_handle::usb_names() const -> std::vector<std::string>
{
  std::vector<std::string> names;
  for(auto const& unit : engine().description().peripherals)
  {
    auto* const port = dynamic_cast<periph::stm32::usb_host_port*>(engine().model(unit.name));
    if(not unit.alias_of and port != nullptr and port->board_host() != nullptr)
    {
      names.push_back(unit.name);
    }
  }
  return names;
}

auto machine_handle::attach_usb(std::string const& peripheral) -> usb_link
{
  auto* const port = dynamic_cast<periph::stm32::usb_host_port*>(engine().model(peripheral));
  if(port == nullptr or port->board_host() == nullptr)
  {
    throw model_error(peripheral, "", 0);
  }
  auto const hubs = hubs_.find(peripheral);
  return usb_link{*port, hubs != hubs_.end() ? hubs->second : detail::usb_hubs{}, alive_, slot_};
}
namespace
{
template <typename Hub, typename Test>
auto channel_predicate(
    std::shared_ptr<Hub>      hub,
    std::size_t               function,
    detail::alive_flag const& alive,
    detail::host_ref          machine,
    Test                      test) -> native_predicate
{
  if(not detail::is_alive(alive))
  {
    throw config_error("usb", 0, "machine", "a USB host whose machine still exists");
  }
  return native_predicate{
      [hub = std::move(hub), function, alive, test = std::move(test)] {
        return not detail::is_alive(alive)
            or detail::link_locked(*hub, [&](core::link_guard const& held) {
                 return test(hub->channel(held, function));
               });
      },
      std::move(machine)};
}
}

auto cdc_link::predicate_available(std::uint64_t count) const -> native_predicate
{
  return channel_predicate(
             hub_, function_, alive_, host_,
             [count](usb::cdc_channel const& queues) { return queues.in_bytes >= count; })
      .described("CDC data");
}

auto cdc_link::predicate_changed(count_mark const& mark) const -> native_predicate
{
  return channel_predicate(
             hub_, function_, alive_, host_,
             [mark = mark.shared()](usb::cdc_channel const& queues) {
               return queues.in_bytes != mark->load(std::memory_order_acquire);
             })
      .described("CDC data");
}

auto cdc_link::predicate_packets(std::uint64_t count) const -> native_predicate
{
  return channel_predicate(
             hub_, function_, alive_, host_,
             [count](usb::cdc_channel const& queues) { return queues.packets() >= count; })
      .described("CDC packets");
}

auto cdc_link::predicate_ready() const -> native_predicate
{
  return channel_predicate(
             hub_, function_, alive_, host_,
             [](usb::cdc_channel const& queues) {
               return queues.comm_interface.has_value() and queues.ready;
             })
      .described("the CDC function to be ready");
}

auto cdc_link::predicate_ended(std::uint32_t id) const -> native_predicate
{
  return channel_predicate(
             hub_, function_, alive_, host_,
             [id](usb::cdc_channel const& queues) { return queues.results.contains(id); })
      .described("a CDC request to end");
}

auto hid_link::predicate_available(std::uint64_t count) const -> native_predicate
{
  return channel_predicate(
             hub_, function_, alive_, host_,
             [count](usb::hid_channel const& queues) { return queues.in.size() >= count; })
      .described("HID input reports");
}

auto hid_link::predicate_ready() const -> native_predicate
{
  return channel_predicate(
             hub_, function_, alive_, host_,
             [](usb::hid_channel const& queues) {
               return queues.interface.has_value() and queues.ready;
             })
      .described("the HID function to be ready");
}

auto hid_link::predicate_ended(std::uint32_t id) const -> native_predicate
{
  return channel_predicate(
             hub_, function_, alive_, host_,
             [id](usb::hid_channel const& queues) { return queues.results.contains(id); })
      .described("a HID request to end");
}

auto hid_link::predicate_written(std::uint64_t id) const -> native_predicate
{
  return channel_predicate(
             hub_, function_, alive_, host_,
             [id](usb::hid_channel const& queues) { return queues.written.contains(id); })
      .described("a HID output report to end");
}

auto uac_link::predicate_available(std::uint64_t count) const -> native_predicate
{
  return channel_predicate(
             hub_, function_, alive_, host_,
             [count](usb::uac2_channel const& queues) { return queues.mic.in.size() >= count; })
      .described("microphone frames");
}

auto uac_link::predicate_ready() const -> native_predicate
{
  return channel_predicate(
             hub_, function_, alive_, host_,
             [](usb::uac2_channel const& queues) {
               return queues.interface.has_value() and queues.ready;
             })
      .described("the audio function to be ready");
}

auto uac_link::predicate_ended(std::uint64_t id) const -> native_predicate
{
  return channel_predicate(
             hub_, function_, alive_, host_,
             [id](usb::uac2_channel const& queues) { return queues.results.contains(id); })
      .described("an audio request to end");
}

auto uac_link::predicate_drained(std::uint64_t flushes) const -> native_predicate
{
  return channel_predicate(
             hub_, function_, alive_, host_,
             [flushes](usb::uac2_channel const& queues) {
               return (queues.speaker.out.empty() and not queues.speaker.sending)
                   or queues.speaker.flushes != flushes;
             })
      .described("the speaker to drain");
}

auto usb_link::predicate_settled() const -> native_predicate
{
  std::ignore = port();
  return native_predicate{
      [port = port_, alive = alive_] {
        if(not detail::is_alive(alive))
        {
          return true;
        }
        auto const& on = *port->board_host();
        return on.state() == usb::device_state::configured or on.failed_step().has_value();
      },
      host_}
      .described("the USB enumeration to settle");
}

auto usb_link::predicate_control_ended(std::uint32_t id) const -> native_predicate
{
  std::ignore = port();
  return native_predicate{
      [port = port_, alive = alive_, id] {
        return not detail::is_alive(alive) or port->board_host()->has_result(id);
      },
      host_}
      .described("a control transfer to end");
}
auto usb_link::op_settled() const -> access_op
{
  std::ignore = port();
  return detail::link_op([port = port_, alive = alive_] {
    if(not detail::is_alive(alive))
    {
      throw config_error("usb", 0, "machine", "a USB host whose machine still exists");
    }
    auto const&                on     = *port->board_host();
    auto const                 state  = std::to_underlying(on.state());
    auto const                 failed = on.failed_step();
    std::optional<std::string> step;
    if(failed)
    {
      step = std::string{reflex::enum_value_name(*failed)};
    }
    return std::function<reflex::py::nb::object()>{
        [state, step] { return reflex::py::nb::object{reflex::py::nb::make_tuple(state, step)}; }};
  });
}

auto usb_link::op_take_result(usb_control_ticket const& ticket) const -> access_op
{
  std::ignore = port();
  return detail::link_op([port = port_, alive = alive_, claim = ticket.claim()] {
    if(not detail::is_alive(alive))
    {
      throw config_error("usb", 0, "machine", "a USB host whose machine still exists");
    }
    std::optional<usb_control_result> found;
    auto&                             on = *port->board_host();
    auto const                        taken =
        claim->id and on.epoch() == claim->epoch ? on.take_result(*claim->id) : std::nullopt;
    if(taken)
    {
      claim->taken = true;
      found        = usb_control_result{
          .id     = taken->id,
          .status = taken->status,
          .data   = taken->data,
          .at_ps  = taken->at.ps,
      };
    }
    return std::function<reflex::py::nb::object()>{
        [found = std::move(found)] { return reflex::py::nb::cast(found); }};
  });
}
}
