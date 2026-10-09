#include <popcorn/periph/usb_cdc_acm.hpp>

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
constexpr std::uint8_t  cdc_class          = 0x02;
constexpr std::uint8_t  acm_subclass       = 0x02;
constexpr std::uint8_t  cs_interface       = 0x24;
constexpr std::uint8_t  union_subtype      = 0x06;
constexpr std::size_t   union_head         = 3;
constexpr std::uint8_t  class_to_interface = 0x21;
constexpr std::uint8_t  set_line_coding    = 0x20;
constexpr std::uint8_t  set_line_state     = 0x22;
constexpr std::uint8_t  serial_state       = 0x20;
constexpr std::size_t   coding_length      = 7;
constexpr std::size_t   notification_head  = 8;
constexpr std::size_t   write_packets      = 20;
constexpr std::uint8_t  to_endpoint        = 0x02;
constexpr std::uint8_t  clear_feature      = 0x01;
constexpr std::uint16_t endpoint_halt      = 0;

auto bulk_pair(interface_info const& setting) -> bool
{
  auto const bulk = [&](bool in) {
    return std::ranges::any_of(setting.endpoints, [&](endpoint_info const& endpoint) {
      return endpoint.type == transfer_type::bulk and endpoint.in() == in;
    });
  };
  return bulk(true) and bulk(false);
}

auto union_members(interface_info const& comm) -> std::vector<std::uint8_t>
{
  std::span<std::uint8_t const> const extra = comm.extra;
  for(std::size_t at = 0; at + 2 <= extra.size();)
  {
    auto const length = std::size_t{extra[at]};
    if(length < 2 or at + length > extra.size())
    {
      break;
    }
    if(extra[at + 1] == cs_interface and length > union_head + 1 and extra[at + 2] == union_subtype)
    {
      auto const members = extra.subspan(at + union_head, length - union_head);
      return {members.begin(), members.end()};
    }
    at += length;
  }
  return {};
}

auto encode(line_coding const& coding) -> std::vector<std::uint8_t>
{
  return {
      static_cast<std::uint8_t>(coding.baud),
      static_cast<std::uint8_t>(coding.baud >> 8),
      static_cast<std::uint8_t>(coding.baud >> 16),
      static_cast<std::uint8_t>(coding.baud >> 24),
      coding.stop_bits,
      coding.parity,
      coding.data_bits};
}

auto decode(std::span<std::uint8_t const> bytes) -> line_coding
{
  return {
      .baud      = static_cast<std::uint32_t>(bytes[0])
                 | static_cast<std::uint32_t>(bytes[1]) << 8
                 | static_cast<std::uint32_t>(bytes[2]) << 16
                 | static_cast<std::uint32_t>(bytes[3]) << 24,
      .stop_bits = bytes[4],
      .parity    = bytes[5],
      .data_bits = bytes[6],
  };
}

void trim(cdc_channel& channel)
{
  while(channel.in_bytes > channel.limit)
  {
    auto const& front = channel.in.front();
    auto const  count = std::min<std::uint64_t>(
        front.data.size() - channel.taken, channel.in_bytes - channel.limit);
    channel.taken += static_cast<std::size_t>(count);
    channel.in_bytes -= count;
    channel.dropped += count;
    if(channel.taken == front.data.size())
    {
      channel.in.pop_front();
      channel.taken = 0;
    }
  }
}

struct driver_state
{
  std::uint64_t          function{};
  std::uint8_t           comm{};
  std::uint8_t           data{};
  pipe_id                notify{};
  pipe_id                bulk_in{};
  pipe_id                bulk_out{};
  std::uint16_t          notify_packet{};
  std::uint16_t          in_packet{};
  std::uint16_t          out_packet{};
  std::uint8_t           in_endpoint{};
  transfer_id            line_state_request{};
  transfer_id            clear_halt_request{};
  bool                   ready{};
  bool                   out_halted{};
  std::uint64_t          in_flight{};
  std::vector<std::byte> out{};
  std::vector<std::byte> notification{};
  line_coding            coding{};
  std::uint8_t           line_state{};
  std::uint16_t          serial_state{};
};

class cdc_acm_driver final : public class_driver
{
public:
  cdc_acm_driver(std::shared_ptr<cdc_hub> hub, cdc_options const& options)
      : hub_{std::move(hub)}, options_{options}
  {}

  cdc_acm_driver(cdc_acm_driver const&)                    = delete;
  auto operator=(cdc_acm_driver const&) -> cdc_acm_driver& = delete;
  cdc_acm_driver(cdc_acm_driver&&)                         = delete;
  auto operator=(cdc_acm_driver&&) -> cdc_acm_driver&      = delete;

  ~cdc_acm_driver() override
  {
    if(channel_ == nullptr)
    {
      return;
    }
    auto const held = hub_->lock();
    channel_->comm_interface.reset();
    channel_->ready     = false;
    channel_->in_flight = 0;
    for(auto const id : channel_->awaited)
    {
      channel_->results[id] = transfer_status::cancelled;
    }
    channel_->awaited.clear();
  }

  void configured(host_context& ctx) override
  {
    deferred_warnings const deferred;
    auto const              held          = hub_->lock();
    auto const&             configuration = ctx.configuration();
    auto const              claimed       = ctx.interfaces();
    s_.comm                               = claimed.front();
    auto const functions                  = cdc_acm_functions(configuration);
    s_.function =
        static_cast<std::uint64_t>(std::ranges::find(functions, s_.comm) - functions.begin());
    bind(held);
    if(auto const* const comm = configuration.find(s_.comm))
    {
      for(auto const& endpoint : comm->endpoints)
      {
        if(endpoint.type == transfer_type::interrupt and endpoint.in() and s_.notify == 0)
        {
          s_.notify        = ctx.open_pipe(endpoint);
          s_.notify_packet = endpoint.max_packet;
        }
      }
    }
    for(auto const number : claimed.subspan(1))
    {
      auto const* const setting = configuration.find(number);
      if(setting == nullptr or not bulk_pair(*setting))
      {
        continue;
      }
      s_.data = number;
      for(auto const& endpoint : setting->endpoints)
      {
        if(endpoint.type != transfer_type::bulk)
        {
          continue;
        }
        auto& pipe   = endpoint.in() ? s_.bulk_in : s_.bulk_out;
        auto& packet = endpoint.in() ? s_.in_packet : s_.out_packet;
        if(pipe == 0)
        {
          pipe   = ctx.open_pipe(endpoint);
          packet = endpoint.max_packet;
          if(endpoint.in())
          {
            s_.in_endpoint = endpoint.address;
          }
        }
      }
      break;
    }
    if(s_.notify != 0)
    {
      ctx.submit(s_.notify, {.length = s_.notify_packet});
    }
    ctx.submit(s_.bulk_in, {.length = s_.in_packet});
    ctx.control(
        {.request_type = class_to_interface,
         .request      = set_line_coding,
         .value        = 0,
         .index        = s_.comm,
         .length       = coding_length},
        encode(options_.coding));
    auto const lines =
        static_cast<std::uint16_t>((options_.dtr ? 1u : 0u) | (options_.rts ? 2u : 0u));
    s_.line_state_request = ctx.control(
        {.request_type = class_to_interface,
         .request      = set_line_state,
         .value        = lines,
         .index        = s_.comm,
         .length       = 0});
  }

  void control_done(host_context& ctx, transfer_result const& result) override
  {
    deferred_warnings const deferred;
    auto const              held      = hub_->lock();
    auto const              completed = result.status == transfer_status::completed;
    if(completed
       and result.setup.request == set_line_coding
       and result.data.size() >= coding_length)
    {
      s_.coding = channel_->coding = decode(result.data);
    }
    else if(completed and result.setup.request == set_line_state)
    {
      s_.line_state = channel_->line_state = static_cast<std::uint8_t>(result.setup.value & 3u);
    }
    if(channel_->awaited.erase(result.id) != 0)
    {
      channel_->results[result.id] = result.status;
    }
    if(result.id == s_.line_state_request)
    {
      s_.ready = channel_->ready = true;
      send(ctx);
    }
    if(result.id == s_.clear_halt_request)
    {
      s_.clear_halt_request = 0;
      if(not completed)
      {
        ctx.warn(
            std::format(
                "USB CDC ACM function {}: CLEAR_FEATURE(ENDPOINT_HALT) on bulk IN ended {}, "
                "reading "
                "stops",
                s_.function, reflex::enum_value_name(result.status)));
        return;
      }
      ctx.reset_toggle(s_.bulk_in);
      ctx.submit(s_.bulk_in, {.length = s_.in_packet});
    }
  }

  void transfer_done(host_context& ctx, pipe_id pipe, transfer_result const& result) override
  {
    deferred_warnings const deferred;
    auto const              held    = hub_->lock();
    auto const              stalled = result.status == transfer_status::stalled;
    if(pipe == s_.bulk_in)
    {
      channel_->received(result.data, result.at);
      if(stalled)
      {
        s_.clear_halt_request = ctx.control(
            {.request_type = to_endpoint,
             .request      = clear_feature,
             .value        = endpoint_halt,
             .index        = s_.in_endpoint,
             .length       = 0});
        return;
      }
      ctx.submit(s_.bulk_in, {.length = s_.in_packet});
    }
    else if(pipe == s_.bulk_out)
    {
      if(result.status != transfer_status::completed)
      {
        ctx.warn(
            std::format(
                "USB CDC ACM function {}: bulk OUT ended {}, {} bytes lost{}", s_.function,
                stalled ? "stalled" : "in error", s_.in_flight, stalled ? ", writing stops" : ""));
      }
      s_.out_halted       = stalled;
      s_.in_flight        = 0;
      channel_->in_flight = 0;
      send(ctx);
    }
    else if(pipe == s_.notify)
    {
      notified(result.data);
      if(result.data.size() < s_.notify_packet)
      {
        s_.notification.clear();
      }
      if(not stalled)
      {
        ctx.submit(s_.notify, {.length = s_.notify_packet});
      }
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
    saved.out        = channel_->out
                     | std::views::transform([](std::uint8_t b) { return std::byte{b}; })
                     | std::ranges::to<std::vector>();
    reflex::serde::bson::serializer{out}.dump(saved);
  }

  void load(std::span<std::byte const> in) override
  {
    s_              = reflex::serde::bson::deserializer{in}.template load<driver_state>();
    auto const held = hub_->lock();
    bind(held);
    channel_->out.assign_range(s_.out | std::views::transform([](std::byte b) {
                                 return std::to_integer<std::uint8_t>(b);
                               }));
    s_.out.clear();
    channel_->in_flight    = s_.in_flight;
    channel_->coding       = s_.coding;
    channel_->line_state   = s_.line_state;
    channel_->serial_state = s_.serial_state;
    channel_->ready        = s_.ready;
  }

private:
  void bind(core::link_guard const& held)
  {
    channel_                 = &hub_->channel(held, static_cast<std::size_t>(s_.function));
    channel_->comm_interface = s_.comm;
  }

  void send(host_context& ctx)
  {
    auto& out = channel_->out;
    if(not s_.ready or s_.out_halted or s_.bulk_out == 0 or s_.in_flight != 0 or out.empty())
    {
      return;
    }
    auto const                count = std::min(out.size(), write_packets * s_.out_packet);
    std::vector<std::uint8_t> data(out.begin(), out.begin() + static_cast<std::ptrdiff_t>(count));
    out.erase(out.begin(), out.begin() + static_cast<std::ptrdiff_t>(count));
    s_.in_flight = channel_->in_flight = count;
    ctx.submit(s_.bulk_out, {.data = std::move(data), .length = 0, .zero_packet = true});
  }

  void notified(std::span<std::uint8_t const> data)
  {
    auto& buffer = s_.notification;
    buffer.append_range(data | std::views::transform([](std::uint8_t b) { return std::byte{b}; }));
    while(buffer.size() >= notification_head)
    {
      auto const byte = [&](std::size_t at) { return std::to_integer<std::uint16_t>(buffer[at]); };
      auto const length = static_cast<std::size_t>(byte(6) | byte(7) << 8);
      if(buffer.size() < notification_head + length)
      {
        break;
      }
      if(byte(1) == serial_state and length >= 2)
      {
        s_.serial_state = channel_->serial_state =
            static_cast<std::uint16_t>(byte(notification_head) | byte(notification_head + 1) << 8);
      }
      buffer.erase(
          buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(notification_head + length));
    }
  }

  std::shared_ptr<cdc_hub> hub_;
  cdc_options              options_;
  driver_state             s_{};
  cdc_channel*             channel_{};
};
}

void cdc_channel::received(std::span<std::uint8_t const> data, vtime at)
{
  if(data.empty())
  {
    return;
  }
  in.push_back({
      .data = {data.begin(), data.end()},
        .at = at
  });
  in_bytes += data.size();
  trim(*this);
}

auto cdc_channel::take(std::uint64_t bytes) -> std::vector<cdc_packet>
{
  std::vector<cdc_packet> out;
  while(bytes != 0 and not in.empty())
  {
    auto const& front = in.front();
    auto const  count = std::min<std::uint64_t>(front.data.size() - taken, bytes);
    auto const  first = front.data.begin() + static_cast<std::ptrdiff_t>(taken);
    out.push_back({
        .data = {first, first + static_cast<std::ptrdiff_t>(count)},
          .at = front.at
    });
    taken += static_cast<std::size_t>(count);
    bytes -= count;
    in_bytes -= count;
    if(taken == front.data.size())
    {
      in.pop_front();
      taken = 0;
    }
  }
  return out;
}

auto cdc_channel::peek(std::uint64_t start, std::uint64_t count) const -> std::vector<std::uint8_t>
{
  std::vector<std::uint8_t> out;
  auto                      skip = taken + start;
  for(auto const& packet : in)
  {
    if(out.size() == count)
    {
      break;
    }
    if(skip >= packet.data.size())
    {
      skip -= packet.data.size();
      continue;
    }
    auto const first = packet.data.begin() + static_cast<std::ptrdiff_t>(skip);
    auto const kept  = std::min<std::uint64_t>(
        count - out.size(), static_cast<std::uint64_t>(packet.data.end() - first));
    out.insert(out.end(), first, first + static_cast<std::ptrdiff_t>(kept));
    skip = 0;
  }
  return out;
}

auto cdc_channel::take_packets(std::uint64_t count) -> std::vector<cdc_packet>
{
  std::vector<cdc_packet> out;
  while(count-- != 0 and not in.empty())
  {
    auto packet = take(in.front().data.size() - taken);
    out.push_back(std::move(packet.front()));
  }
  return out;
}

auto cdc_channel::packets() const noexcept -> std::uint64_t
{
  return in.size();
}

void cdc_channel::set_limit(std::uint64_t bytes)
{
  limit = bytes;
  trim(*this);
}

void cdc_channel::discard()
{
  in.clear();
  taken     = 0;
  in_bytes  = 0;
  dropped   = 0;
  in_flight = 0;
  out.clear();
  coding       = {};
  line_state   = 0;
  serial_state = 0;
  ready        = false;
  awaited.clear();
  results.clear();
}

auto cdc_hub::channel(core::link_guard const&, std::size_t function) -> cdc_channel&
{
  return channels_[function];
}

namespace
{
struct pending_out
{
  std::uint64_t          function{};
  std::vector<std::byte> bytes{};
};

struct hub_state
{
  std::vector<pending_out> unbound{};
};
}

void cdc_hub::save(std::vector<std::byte>& out) const
{
  auto const held = lock();
  hub_state  saved;
  for(auto const& [function, channel] : channels_)
  {
    if(not channel.comm_interface and not channel.out.empty())
    {
      saved.unbound.push_back(
          {.function = function,
           .bytes    = channel.out
                     | std::views::transform([](std::uint8_t b) { return std::byte{b}; })
                     | std::ranges::to<std::vector>()});
    }
  }
  reflex::serde::bson::serializer{out}.dump(saved);
}

void cdc_hub::load(std::span<std::byte const> in)
{
  auto const held = lock();
  for(auto& channel : channels_ | std::views::values)
  {
    channel.out.clear();
  }
  if(in.empty())
  {
    return;
  }
  for(auto const& pending :
      reflex::serde::bson::deserializer{in}.template load<hub_state>().unbound)
  {
    channel(held, static_cast<std::size_t>(pending.function))
        .out.assign_range(pending.bytes | std::views::transform([](std::byte b) {
                            return std::to_integer<std::uint8_t>(b);
                          }));
  }
}

void cdc_hub::discard()
{
  auto const held = lock();
  for(auto& channel : channels_ | std::views::values)
  {
    channel.discard();
  }
}

auto cdc_acm_claim(configuration_info const& configuration, interface_info const& interface)
    -> std::vector<std::uint8_t>
{
  if(interface.alternate != 0
     or interface.class_code != cdc_class
     or interface.subclass != acm_subclass)
  {
    return {};
  }
  auto members = configuration.function_of(interface.number);
  if(members.size() < 2)
  {
    members = union_members(interface);
  }
  std::vector<std::uint8_t> claimed{interface.number};
  for(auto const number : members)
  {
    if(not std::ranges::contains(claimed, number))
    {
      claimed.push_back(number);
    }
  }
  auto const has_data =
      std::ranges::any_of(claimed | std::views::drop(1), [&](std::uint8_t number) {
        auto const* const setting = configuration.find(number);
        return setting != nullptr and bulk_pair(*setting);
      });
  return has_data ? claimed : std::vector<std::uint8_t>{};
}

auto cdc_acm_functions(configuration_info const& configuration) -> std::vector<std::uint8_t>
{
  return configuration.interfaces
       | std::views::filter([&](interface_info const& setting) {
           return not cdc_acm_claim(configuration, setting).empty();
         })
       | std::views::transform(&interface_info::number)
       | std::ranges::to<std::vector>();
}

auto cdc_acm_factory(std::shared_ptr<cdc_hub> hub, cdc_options options) -> driver_factory
{
  return {
      .name  = std::string{cdc_acm_name},
      .claim = cdc_acm_claim,
      .make  = [hub, options]() -> std::unique_ptr<class_driver> {
        return std::make_unique<cdc_acm_driver>(hub, options);
      },
      .save = [hub](std::vector<std::byte>& out) { hub->save(out); },
      .load = [hub](std::span<std::byte const> in) { hub->load(in); },
  };
}
}
