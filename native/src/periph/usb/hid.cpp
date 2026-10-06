#include <popcorn/periph/usb_hid.hpp>

#include <reflex/enum.hpp>
#include <reflex/serde/bson.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <ranges>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace popcorn::periph::usb
{
namespace
{
constexpr std::uint8_t  hid_class              = 0x03;
constexpr std::uint8_t  hid_descriptor_type    = 0x21;
constexpr std::uint8_t  report_descriptor_type = 0x22;
constexpr std::size_t   hid_descriptor_head    = 6;
constexpr std::size_t   class_descriptor_size  = 3;
constexpr std::uint16_t max_report_descriptor  = 4096;
constexpr std::uint8_t  class_to_interface     = 0x21;
constexpr std::uint8_t  standard_to_host       = 0x81;
constexpr std::uint8_t  get_descriptor         = 0x06;
constexpr std::uint8_t  set_idle               = 0x0a;
constexpr std::uint8_t  set_report             = 0x09;
constexpr std::uint8_t  to_endpoint            = 0x02;
constexpr std::uint8_t  clear_feature          = 0x01;
constexpr std::uint16_t endpoint_halt          = 0;
constexpr std::uint8_t  descriptor_attempts    = 4;
constexpr std::size_t   max_stage              = 0xffff;

auto find_descriptor(std::span<std::uint8_t const> extra, std::uint8_t type)
    -> std::optional<std::span<std::uint8_t const>>
{
  for(std::size_t at = 0; at + 2 <= extra.size();)
  {
    auto const length = std::size_t{extra[at]};
    if(length < 2 or at + length > extra.size())
    {
      break;
    }
    if(extra[at + 1] == type)
    {
      return extra.subspan(at, length);
    }
    at += length;
  }
  return std::nullopt;
}

auto as_bytes(std::span<std::uint8_t const> data) -> std::vector<std::byte>
{
  return data
       | std::views::transform([](std::uint8_t b) { return std::byte{b}; })
       | std::ranges::to<std::vector>();
}

auto as_octets(std::span<std::byte const> data) -> std::vector<std::uint8_t>
{
  return data
       | std::views::transform([](std::byte b) { return std::to_integer<std::uint8_t>(b); })
       | std::ranges::to<std::vector>();
}

void trimmed(hid_channel& channel)
{
  while(channel.in.size() > channel.limit)
  {
    channel.in.pop_front();
    ++channel.dropped;
  }
}

struct saved_write
{
  std::uint64_t          id{};
  std::vector<std::byte> data{};
};

auto saved_writes(std::deque<hid_write> const& out) -> std::vector<saved_write>
{
  return out
       | std::views::transform([](hid_write const& write) {
           return saved_write{.id = write.id, .data = as_bytes(write.data)};
         })
       | std::ranges::to<std::vector>();
}

void restore_writes(hid_channel& channel, std::span<saved_write const> writes)
{
  for(auto const& write : writes)
  {
    channel.out.push_back({.id = write.id, .data = as_octets(write.data)});
    channel.next_write = std::max(channel.next_write, write.id + 1);
  }
}

struct driver_state
{
  std::uint64_t            function{};
  std::uint8_t             interface{};
  pipe_id                  in_pipe{};
  pipe_id                  out_pipe{};
  std::uint16_t            in_packet{};
  std::uint8_t             in_endpoint{};
  std::uint16_t            descriptor_length{};
  std::uint8_t             attempts{};
  transfer_id              descriptor_request{};
  transfer_id              clear_halt_request{};
  transfer_id              write_request{};
  bool                     ready{};
  std::uint64_t            in_flight{};
  std::vector<std::byte>   descriptor{};
  std::vector<saved_write> out{};
};

class hid_driver final : public class_driver
{
public:
  explicit hid_driver(std::shared_ptr<hid_hub> hub) : hub_{std::move(hub)}
  {}

  hid_driver(hid_driver const&)                    = delete;
  auto operator=(hid_driver const&) -> hid_driver& = delete;
  hid_driver(hid_driver&&)                         = delete;
  auto operator=(hid_driver&&) -> hid_driver&      = delete;

  ~hid_driver() override
  {
    if(channel_ == nullptr)
    {
      return;
    }
    auto const held = hub_->lock();
    channel_->interface.reset();
    channel_->ready = false;
    if(channel_->in_flight != 0 and channel_->awaited_writes.erase(channel_->in_flight) != 0)
    {
      channel_->written[channel_->in_flight] = transfer_status::cancelled;
    }
    channel_->in_flight = 0;
    for(auto const id : channel_->awaited)
    {
      channel_->results[id] = {.status = transfer_status::cancelled, .data = {}};
    }
    channel_->awaited.clear();
  }

  void configured(host_context& ctx) override
  {
    deferred_warnings const deferred;
    auto const              held          = hub_->lock();
    auto const&             configuration = ctx.configuration();
    s_.interface                          = ctx.interfaces().front();
    auto const functions                  = hid_functions(configuration);
    s_.function =
        static_cast<std::uint64_t>(std::ranges::find(functions, s_.interface) - functions.begin());
    bind(held);
    channel_->report_descriptor.clear();
    auto const* const setting = configuration.find(s_.interface);
    for(auto const& endpoint : setting->endpoints)
    {
      if(endpoint.type != transfer_type::interrupt)
      {
        continue;
      }
      if(endpoint.in() and s_.in_pipe == 0)
      {
        s_.in_pipe     = ctx.open_pipe(endpoint);
        s_.in_packet   = endpoint.max_packet;
        s_.in_endpoint = endpoint.address;
      }
      else if(not endpoint.in() and s_.out_pipe == 0)
      {
        s_.out_pipe = ctx.open_pipe(endpoint);
      }
    }
    s_.descriptor_length = hid_report_descriptor_length(*setting).value_or(0);
    ctx.control(
        {.request_type = class_to_interface,
         .request      = set_idle,
         .value        = 0,
         .index        = s_.interface,
         .length       = 0});
    read_descriptor(ctx);
  }

  void control_done(host_context& ctx, transfer_result const& result) override
  {
    deferred_warnings const deferred;
    auto const              held = hub_->lock();
    if(channel_->awaited.erase(result.id) != 0)
    {
      channel_->results[result.id] = {.status = result.status, .data = result.data};
    }
    if(result.id == s_.descriptor_request)
    {
      described(ctx, result);
    }
    else if(result.id == s_.clear_halt_request)
    {
      s_.clear_halt_request = 0;
      if(result.status != transfer_status::completed)
      {
        ctx.warn(
            std::format(
                "USB HID function {}: CLEAR_FEATURE(ENDPOINT_HALT) on interrupt IN ended {}, "
                "reading stops",
                s_.function, reflex::enum_value_name(result.status)));
        return;
      }
      ctx.reset_toggle(s_.in_pipe);
      ctx.submit(s_.in_pipe, {.length = s_.in_packet});
    }
    else if(result.id == s_.write_request)
    {
      s_.write_request = 0;
      written(ctx, result.status);
    }
  }

  void transfer_done(host_context& ctx, pipe_id pipe, transfer_result const& result) override
  {
    deferred_warnings const deferred;
    auto const              held = hub_->lock();
    if(pipe == s_.in_pipe)
    {
      channel_->received(result.data, result.at);
      if(result.status == transfer_status::stalled)
      {
        s_.clear_halt_request = ctx.control(
            {.request_type = to_endpoint,
             .request      = clear_feature,
             .value        = endpoint_halt,
             .index        = s_.in_endpoint,
             .length       = 0});
        return;
      }
      ctx.submit(s_.in_pipe, {.length = s_.in_packet});
    }
    else if(pipe == s_.out_pipe)
    {
      written(ctx, result.status);
    }
  }

  void frame(host_context& ctx, std::uint16_t) override
  {
    deferred_warnings const deferred;
    auto const              held = hub_->lock();
    send(ctx);
  }

  void save(std::vector<std::byte>& out) const override
  {
    auto const held  = hub_->lock();
    auto       saved = s_;
    saved.out        = saved_writes(channel_->out);
    reflex::serde::bson::serializer{out}.dump(saved);
  }

  void load(std::span<std::byte const> in) override
  {
    s_              = reflex::serde::bson::deserializer{in}.template load<driver_state>();
    auto const held = hub_->lock();
    bind(held);
    channel_->out.clear();
    restore_writes(*channel_, s_.out);
    s_.out.clear();
    channel_->in_flight         = s_.in_flight;
    channel_->next_write        = std::max(channel_->next_write, s_.in_flight + 1);
    channel_->report_descriptor = as_octets(s_.descriptor);
    channel_->ready             = s_.ready;
  }

private:
  void bind(core::link_guard const& held)
  {
    channel_            = &hub_->channel(held, static_cast<std::size_t>(s_.function));
    channel_->interface = s_.interface;
  }

  void read_descriptor(host_context& ctx)
  {
    ++s_.attempts;
    s_.descriptor_request = ctx.control(
        {.request_type = standard_to_host,
         .request      = get_descriptor,
         .value        = static_cast<std::uint16_t>(report_descriptor_type << 8),
         .index        = s_.interface,
         .length       = s_.descriptor_length});
  }

  void described(host_context& ctx, transfer_result const& result)
  {
    s_.descriptor_request = 0;
    auto const completed  = result.status == transfer_status::completed;
    if(not(completed and result.data.size() >= s_.descriptor_length)
       and s_.attempts < descriptor_attempts)
    {
      read_descriptor(ctx);
      return;
    }
    if(not completed)
    {
      ctx.warn(
          std::format(
              "USB HID function {}: GET_DESCRIPTOR of the report descriptor ended {} {} times, "
              "the function stays idle",
              s_.function, reflex::enum_value_name(result.status), s_.attempts));
      return;
    }
    s_.descriptor               = as_bytes(result.data);
    channel_->report_descriptor = result.data;
    s_.ready = channel_->ready = true;
    ctx.submit(s_.in_pipe, {.length = s_.in_packet});
    send(ctx);
  }

  void written(host_context& ctx, transfer_status status)
  {
    auto const id       = std::exchange(s_.in_flight, 0);
    channel_->in_flight = 0;
    if(channel_->awaited_writes.erase(id) != 0)
    {
      channel_->written[id] = status;
    }
    else if(status != transfer_status::completed)
    {
      ctx.warn(
          std::format(
              "USB HID function {}: output report {} ended {}", s_.function, id,
              reflex::enum_value_name(status)));
    }
    send(ctx);
  }

  void send(host_context& ctx)
  {
    auto& out = channel_->out;
    if(not s_.ready or s_.in_flight != 0 or out.empty())
    {
      return;
    }
    auto write = std::move(out.front());
    out.pop_front();
    s_.in_flight = channel_->in_flight = write.id;
    auto const report_id               = write.data.empty() ? std::uint8_t{} : write.data.front();
    auto       data                    = std::move(write.data);
    if(report_id == 0 and not data.empty())
    {
      data.erase(data.begin());
    }
    if(data.size() > max_stage)
    {
      ctx.warn(
          std::format(
              "USB HID function {}: output report {} carries {} bytes, above 0xffff, not sent",
              s_.function, write.id, data.size()));
      written(ctx, transfer_status::error);
      return;
    }
    if(s_.out_pipe != 0)
    {
      ctx.submit(s_.out_pipe, {.data = std::move(data), .length = 0});
      return;
    }
    auto const length = static_cast<std::uint16_t>(data.size());
    s_.write_request  = ctx.control(
        {.request_type = class_to_interface,
         .request      = set_report,
         .value        = static_cast<std::uint16_t>(
             std::to_underlying(hid_report_type::output) << 8 | report_id),
         .index  = s_.interface,
         .length = length},
        std::move(data));
  }

  std::shared_ptr<hid_hub> hub_;
  driver_state             s_{};
  hid_channel*             channel_{};
};

struct unbound_channel
{
  std::uint64_t            function{};
  std::vector<saved_write> writes{};
  std::vector<std::byte>   descriptor{};
};

struct hub_state
{
  std::vector<unbound_channel> unbound{};
};
}

void hid_channel::received(std::span<std::uint8_t const> data, vtime at)
{
  if(data.empty())
  {
    return;
  }
  in.push_back({
      .data = {data.begin(), data.end()},
        .at = at
  });
  trimmed(*this);
}

auto hid_channel::take(std::uint64_t count) -> std::vector<hid_report>
{
  std::vector<hid_report> out;
  while(count-- != 0 and not in.empty())
  {
    out.push_back(std::move(in.front()));
    in.pop_front();
  }
  return out;
}

auto hid_channel::write(std::vector<std::uint8_t> data) -> std::uint64_t
{
  auto const id = next_write++;
  out.push_back({.id = id, .data = std::move(data)});
  return id;
}

auto hid_channel::queued() const noexcept -> std::uint64_t
{
  return out.size() + (in_flight != 0 ? 1 : 0);
}

void hid_channel::set_limit(std::uint64_t count)
{
  limit = count;
  trimmed(*this);
}

void hid_channel::discard()
{
  in.clear();
  dropped   = 0;
  in_flight = 0;
  out.clear();
  awaited_writes.clear();
  written.clear();
  report_descriptor.clear();
  ready = false;
  awaited.clear();
  results.clear();
}

auto hid_hub::channel(core::link_guard const&, std::size_t function) -> hid_channel&
{
  return channels_[function];
}

void hid_hub::save(std::vector<std::byte>& out) const
{
  auto const held = lock();
  hub_state  saved;
  for(auto const& [function, channel] : channels_)
  {
    if(not channel.interface and (not channel.out.empty() or not channel.report_descriptor.empty()))
    {
      saved.unbound.push_back(
          {.function   = function,
           .writes     = saved_writes(channel.out),
           .descriptor = as_bytes(channel.report_descriptor)});
    }
  }
  reflex::serde::bson::serializer{out}.dump(saved);
}

void hid_hub::load(std::span<std::byte const> in)
{
  auto const held = lock();
  for(auto& channel : channels_ | std::views::values)
  {
    channel.out.clear();
    channel.report_descriptor.clear();
  }
  if(in.empty())
  {
    return;
  }
  for(auto const& pending :
      reflex::serde::bson::deserializer{in}.template load<hub_state>().unbound)
  {
    auto& restored = channel(held, static_cast<std::size_t>(pending.function));
    restore_writes(restored, pending.writes);
    restored.report_descriptor = as_octets(pending.descriptor);
  }
}

void hid_hub::discard()
{
  auto const held = lock();
  for(auto& channel : channels_ | std::views::values)
  {
    channel.discard();
  }
}

auto hid_report_descriptor_length(interface_info const& interface) -> std::optional<std::uint16_t>
{
  auto found = find_descriptor(interface.extra, hid_descriptor_type);
  if(not found and not interface.endpoints.empty())
  {
    found = find_descriptor(interface.endpoints.front().extra, hid_descriptor_type);
  }
  if(not found or found->size() < hid_descriptor_head + class_descriptor_size)
  {
    return std::nullopt;
  }
  auto const& hid   = *found;
  auto const  count = std::size_t{hid[5]};
  if(count == 0
     or hid.size() != hid_descriptor_head + class_descriptor_size * count
     or hid[6] != report_descriptor_type)
  {
    return std::nullopt;
  }
  auto const length = static_cast<std::uint16_t>(hid[7] | hid[8] << 8);
  if(length == 0 or length > max_report_descriptor)
  {
    return std::nullopt;
  }
  return length;
}

auto hid_claim(configuration_info const&, interface_info const& interface)
    -> std::vector<std::uint8_t>
{
  auto const interrupt_in =
      std::ranges::any_of(interface.endpoints, [](endpoint_info const& endpoint) {
        return endpoint.type == transfer_type::interrupt and endpoint.in();
      });
  if(interface.alternate != 0
     or interface.class_code != hid_class
     or not interrupt_in
     or not hid_report_descriptor_length(interface))
  {
    return {};
  }
  return {interface.number};
}

auto hid_functions(configuration_info const& configuration) -> std::vector<std::uint8_t>
{
  return configuration.interfaces
       | std::views::filter([&](interface_info const& setting) {
           return not hid_claim(configuration, setting).empty();
         })
       | std::views::transform(&interface_info::number)
       | std::ranges::to<std::vector>();
}

auto hid_factory(std::shared_ptr<hid_hub> hub) -> driver_factory
{
  return {
      .name  = std::string{hid_name},
      .claim = hid_claim,
      .make  = [hub]() -> std::unique_ptr<class_driver> {
        return std::make_unique<hid_driver>(hub);
      },
      .save = [hub](std::vector<std::byte>& out) { hub->save(out); },
      .load = [hub](std::span<std::byte const> in) { hub->load(in); },
  };
}
}
