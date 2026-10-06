#include "usb.hpp"
#include "gil_locks.hpp"
#include "machine.hpp"

#include <popcorn/core/errors.hpp>

#include <reflex/enum.hpp>

#include <algorithm>
#include <cstddef>
#include <format>
#include <functional>
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

void cdc_link::send(reflex::py::nb::bytes const& data)
{
  auto const bytes = std::span{static_cast<std::uint8_t const*>(data.data()), data.size()};
  locked([&](usb::cdc_channel& queues) { queues.out.append_range(bytes); });
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

auto cdc_link::drain(std::uint64_t count) -> reflex::py::nb::bytes
{
  auto const bytes = locked([&](usb::cdc_channel& queues) {
    std::vector<std::uint8_t> taken;
    for(auto const& packet : queues.take(count))
    {
      taken.append_range(packet.data);
    }
    return taken;
  });
  return bytes_of(bytes);
}

auto cdc_link::drain_packets(std::uint64_t count) -> std::vector<cdc_packet_record>
{
  auto const taken = locked([&](usb::cdc_channel& queues) { return queues.take_packets(count); });
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
  locked([&](usb::cdc_channel& queues) {
    queues.awaited.erase(id);
    queues.results.erase(id);
  });
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

auto hid_link::write(reflex::py::nb::bytes const& data) -> std::uint64_t
{
  return locked([&](usb::hid_channel& queues) {
    auto const* const bytes = static_cast<std::uint8_t const*>(data.data());
    auto const        stage = data.size() == 0 ? 0 : data.size() - (bytes[0] == 0 ? 1 : 0);
    if(data.size() < 2 or stage > 0xffff)
    {
      throw config_error(
          "usb", 0, "report",
          std::format(
              "a report of at least 2 bytes, at most 0xffff on the bus, not {} bytes",
              data.size()));
    }
    auto const id = queues.write({bytes, bytes + data.size()});
    queues.awaited_writes.insert(id);
    return id;
  });
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

void hid_link::forget_written(std::uint64_t id)
{
  locked([&](usb::hid_channel& queues) {
    queues.awaited_writes.erase(id);
    queues.written.erase(id);
  });
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
  locked([&](usb::hid_channel& queues) {
    queues.awaited.erase(id);
    queues.results.erase(id);
  });
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
  locked([&](usb::uac2_channel& queues) { queues.speaker.out.append_range(samples); });
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
  return engine([&]() -> std::uint64_t {
    auto* const owner = host();
    if(owner == nullptr)
    {
      return 0;
    }
    return usb::uac2_start(
        *owner, *hub_, function_, mic ? usb::uac2_direction::mic : usb::uac2_direction::speaker);
  });
}

void uac_link::stop(bool mic)
{
  engine([&] {
    if(auto* const owner = host())
    {
      usb::uac2_stop(
          *owner, *hub_, function_, mic ? usb::uac2_direction::mic : usb::uac2_direction::speaker);
    }
  });
}

auto uac_link::set_feature(
    std::uint8_t unit,
    std::uint8_t channel,
    std::uint8_t control,
    std::int32_t value) -> std::uint64_t
{
  return engine([&]() -> std::uint64_t {
    auto* const owner = host();
    if(owner == nullptr
       or (control != std::to_underlying(usb::uac2_control::mute)
           and control != std::to_underlying(usb::uac2_control::volume)))
    {
      return 0;
    }
    return usb::uac2_set_feature(
        *owner, *hub_, function_, unit, channel, static_cast<usb::uac2_control>(control), value);
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
  locked([&](usb::uac2_channel& queues) {
    queues.awaited.erase(id);
    queues.results.erase(id);
  });
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
  auto const* const         first = static_cast<std::uint8_t const*>(data.data());
  std::vector<std::uint8_t> stage(first, first + data.size());
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

auto usb_link::requests() const -> std::vector<usb_request_record>
{
  return engine([&]() -> std::vector<usb_request_record> {
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
  for(auto const& unit : engine_->description().peripherals)
  {
    auto* const port = dynamic_cast<periph::stm32::usb_host_port*>(engine_->model(unit.name));
    if(not unit.alias_of and port != nullptr and port->board_host() != nullptr)
    {
      names.push_back(unit.name);
    }
  }
  return names;
}

auto machine_handle::attach_usb(std::string const& peripheral) -> usb_link
{
  auto* const port = dynamic_cast<periph::stm32::usb_host_port*>(engine_->model(peripheral));
  if(port == nullptr or port->board_host() == nullptr)
  {
    throw model_error(peripheral, "", 0);
  }
  auto const hubs = hubs_.find(peripheral);
  return usb_link{*port, hubs != hubs_.end() ? hubs->second : detail::usb_hubs{}, alive_, slot_};
}
}
