#include <popcorn/periph/usb_uac2.hpp>

#include <reflex/serde/bson.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <initializer_list>
#include <map>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace popcorn::periph::usb
{
namespace
{
constexpr std::uint8_t  audio_class        = 0x01;
constexpr std::uint8_t  audio_control      = 0x01;
constexpr std::uint8_t  audio_streaming    = 0x02;
constexpr std::uint8_t  uac2_protocol      = 0x20;
constexpr std::uint8_t  cs_interface       = 0x24;
constexpr std::uint8_t  input_terminal     = 0x02;
constexpr std::uint8_t  output_terminal    = 0x03;
constexpr std::uint8_t  mixer_unit         = 0x04;
constexpr std::uint8_t  selector_unit      = 0x05;
constexpr std::uint8_t  feature_unit       = 0x06;
constexpr std::uint8_t  effect_unit        = 0x07;
constexpr std::uint8_t  processing_unit    = 0x08;
constexpr std::uint8_t  extension_unit     = 0x09;
constexpr std::uint8_t  clock_source       = 0x0a;
constexpr std::uint8_t  clock_selector     = 0x0b;
constexpr std::uint8_t  clock_multiplier   = 0x0c;
constexpr std::uint8_t  as_general         = 0x01;
constexpr std::uint8_t  format_type        = 0x02;
constexpr std::uint8_t  format_type_i      = 0x01;
constexpr std::uint32_t pcm                = 0x01;
constexpr std::uint8_t  request_cur        = 0x01;
constexpr std::uint8_t  request_range      = 0x02;
constexpr std::uint8_t  class_in           = 0xa1;
constexpr std::uint8_t  class_out          = 0x21;
constexpr std::uint8_t  standard_interface = 0x01;
constexpr std::uint8_t  set_interface_code = 0x0b;
constexpr std::uint8_t  sampling_frequency = 1;
constexpr std::uint8_t  clock_validity     = 2;
constexpr std::size_t   subrange_size      = 12;
constexpr std::size_t   max_rates          = 1024;
constexpr std::size_t   max_subranges      = (0xffff - 2) / subrange_size;
constexpr std::uint8_t  mixer_attempts     = 10;
constexpr std::size_t   max_channels       = 32;
constexpr std::uint32_t frames_per_second  = 1000;
constexpr std::size_t   general_length     = 16;
constexpr std::size_t   format_length      = 6;
constexpr std::size_t   input_length       = 17;
constexpr std::size_t   output_length      = 12;
constexpr std::size_t   clock_length       = 8;
constexpr std::size_t   feature_head       = 6;
constexpr std::size_t   control_bytes      = 4;
constexpr int           clock_hops         = 8;

auto le16(std::span<std::uint8_t const> bytes, std::size_t at) -> std::uint16_t
{
  return at + 2 <= bytes.size() ? static_cast<std::uint16_t>(bytes[at] | bytes[at + 1] << 8) : 0;
}

auto le32(std::span<std::uint8_t const> bytes, std::size_t at) -> std::uint32_t
{
  std::uint32_t value{};
  for(std::size_t i = 0; i < 4; ++i)
  {
    if(at + i < bytes.size())
    {
      value |= std::uint32_t{bytes[at + i]} << (8 * i);
    }
  }
  return value;
}

auto readable(std::uint32_t controls, std::uint8_t control) -> bool
{
  return ((controls >> ((control - 1) * 2)) & 1u) != 0;
}

auto writable(std::uint32_t controls, std::uint8_t control) -> bool
{
  return ((controls >> ((control - 1) * 2)) & 2u) != 0;
}

auto signed_of(std::uint32_t raw, std::size_t bytes) -> std::int32_t
{
  auto const shift = static_cast<int>(32 - 8 * bytes);
  return static_cast<std::int32_t>(raw << shift) >> shift;
}

struct entity
{
  std::uint8_t              subtype{};
  std::vector<std::uint8_t> bytes{};
};

auto entities_of(std::span<std::uint8_t const> extra) -> std::map<std::uint8_t, entity>
{
  std::map<std::uint8_t, entity> found;
  for(std::size_t at = 0; at + 2 <= extra.size();)
  {
    auto const length = std::size_t{extra[at]};
    if(length < 2 or at + length > extra.size())
    {
      break;
    }
    auto const descriptor = extra.subspan(at, length);
    if(descriptor[1] == cs_interface and length >= 4 and descriptor[2] >= input_terminal)
    {
      found.try_emplace(
          descriptor[3],
          entity{
              .subtype = descriptor[2], .bytes = {descriptor.begin(), descriptor.end()}
      });
    }
    at += length;
  }
  return found;
}

auto find_descriptor(std::span<std::uint8_t const> extra, std::uint8_t subtype, std::size_t minimum)
    -> std::optional<std::span<std::uint8_t const>>
{
  for(std::size_t at = 0; at + 2 <= extra.size();)
  {
    auto const length = std::size_t{extra[at]};
    if(length < 2 or at + length > extra.size())
    {
      break;
    }
    if(extra[at + 1] == cs_interface and length >= 3 and extra[at + 2] == subtype)
    {
      if(length < minimum)
      {
        return std::nullopt;
      }
      return extra.subspan(at, length);
    }
    at += length;
  }
  return std::nullopt;
}

auto output_terminals(std::span<std::uint8_t const> extra) -> std::vector<std::vector<std::uint8_t>>
{
  std::vector<std::vector<std::uint8_t>> found;
  for(std::size_t at = 0; at + 2 <= extra.size();)
  {
    auto const length = std::size_t{extra[at]};
    if(length < 2 or at + length > extra.size())
    {
      break;
    }
    if(extra[at + 1] == cs_interface
       and length >= output_length
       and extra[at + 2] == output_terminal)
    {
      found.emplace_back(
          extra.begin() + static_cast<std::ptrdiff_t>(at),
          extra.begin() + static_cast<std::ptrdiff_t>(at + length));
    }
    at += length;
  }
  return found;
}

struct clock_found
{
  std::uint8_t id{};
  std::uint8_t controls{};
};

auto clock_of(std::map<std::uint8_t, entity> const& entities, std::uint8_t id)
    -> std::optional<clock_found>
{
  for(int hop = 0; hop < clock_hops; ++hop)
  {
    auto const found = entities.find(id);
    if(found == entities.end())
    {
      return std::nullopt;
    }
    auto const& bytes = found->second.bytes;
    switch(found->second.subtype)
    {
      case clock_source:
        if(bytes.size() < clock_length)
        {
          return std::nullopt;
        }
        return clock_found{.id = id, .controls = bytes[5]};
      case clock_selector:
        if(bytes.size() < 6 or bytes[4] == 0)
        {
          return std::nullopt;
        }
        id = bytes[5];
        break;
      case clock_multiplier:
        if(bytes.size() < 5)
        {
          return std::nullopt;
        }
        id = bytes[4];
        break;
      default:
        return std::nullopt;
    }
  }
  return std::nullopt;
}

struct alt_state
{
  std::uint8_t               interface{};
  std::uint8_t               alternate{};
  std::uint8_t               endpoint{};
  std::uint16_t              max_packet{};
  std::uint8_t               channels{};
  std::uint8_t               subslot{};
  std::uint8_t               bits{};
  std::uint8_t               clock{};
  std::uint8_t               clock_controls{};
  bool                       usable{};
  std::vector<std::uint32_t> rates{};
  std::uint32_t              current{};
};

struct unit_state
{
  std::uint8_t               id{};
  std::uint8_t               channels{};
  std::vector<std::uint32_t> controls{};
  std::vector<std::int32_t>  mute{};
  std::vector<std::int32_t>  volume{};
  std::vector<std::int32_t>  range_min{};
  std::vector<std::int32_t>  range_max{};
  std::vector<std::int32_t>  range_res{};
  std::uint64_t              mute_read{};
  std::uint64_t              volume_read{};
  std::uint64_t              range_read{};
};

struct stream_state
{
  bool          present{};
  std::uint32_t alt{};
  pipe_id       pipe{};
  bool          running{};
  bool          starting{};
  std::uint64_t operation{};
  std::uint32_t rate{};
  std::uint32_t accumulator{};
  bool          carried{};
};

enum class step_kind : std::uint8_t
{
  range_head,
  range,
  set_interface,
  clock_valid,
  get_rate,
  set_rate,
  get_rate_after,
  clock_valid_after,
  control_begin,
  control_range,
  resolution_save,
  resolution_test,
  resolution_check,
  resolution_restore,
  control_init,
  control_set,
  probe_done,
  start_begin,
  start_interface,
  set_feature,
};

struct step
{
  step_kind     kind{};
  std::uint32_t alt{};
  std::uint8_t  interface{};
  std::uint8_t  alternate{};
  std::uint8_t  unit{};
  std::uint8_t  control{};
  std::uint8_t  channel{};
  std::uint8_t  field{};
  std::uint8_t  attempt{};
  std::uint8_t  direction{};
  std::uint32_t mask{};
  std::uint32_t readonly{};
  std::int32_t  value{};
  std::uint64_t operation{};
};

struct control_state
{
  std::uint8_t  unit{};
  std::uint8_t  control{};
  std::uint32_t mask{};
  std::uint32_t readonly{};
  std::int32_t  min{};
  std::int32_t  max{1};
  std::int32_t  res{1};
  std::int32_t  last_valid{1};
  std::int32_t  saved{};
  std::int32_t  test{};
  std::uint64_t cached{};
};

struct driver_state
{
  std::uint64_t                function{};
  std::uint8_t                 interface{};
  std::vector<alt_state>       alts{};
  std::vector<unit_state>      units{};
  std::vector<stream_state>    streams{2};
  std::vector<step>            steps{};
  bool                         busy{};
  step                         current{};
  transfer_id                  request{};
  bool                         abandoned{};
  control_state                control{};
  bool                         ready{};
  std::optional<std::uint64_t> sent_at{};
  std::vector<std::byte>       out{};
};

auto samples_to_bytes(std::deque<std::int32_t> const& samples) -> std::vector<std::byte>
{
  std::vector<std::byte> out;
  out.reserve(samples.size() * 4);
  for(auto const sample : samples)
  {
    auto const raw = static_cast<std::uint32_t>(sample);
    for(int i = 0; i < 4; ++i)
    {
      out.push_back(static_cast<std::byte>(raw >> (8 * i)));
    }
  }
  return out;
}

auto bytes_to_samples(std::span<std::byte const> bytes) -> std::deque<std::int32_t>
{
  std::deque<std::int32_t> out;
  for(std::size_t at = 0; at + 4 <= bytes.size(); at += 4)
  {
    std::uint32_t raw{};
    for(std::size_t i = 0; i < 4; ++i)
    {
      raw |= std::to_integer<std::uint32_t>(bytes[at + i]) << (8 * i);
    }
    out.push_back(static_cast<std::int32_t>(raw));
  }
  return out;
}

auto channels_of(std::uint32_t mask) -> std::vector<std::uint8_t>
{
  if(mask == 0)
  {
    return {0};
  }
  std::vector<std::uint8_t> out;
  for(std::uint8_t bit = 0; bit < max_channels; ++bit)
  {
    if((mask & (std::uint32_t{1} << bit)) != 0)
    {
      out.push_back(static_cast<std::uint8_t>(bit + 1));
    }
  }
  return out;
}

auto value_size(std::uint8_t control) -> std::uint16_t
{
  return control == std::to_underlying(uac2_control::mute) ? 1 : 2;
}

auto direction_index(uac2_direction direction) -> std::size_t
{
  return std::to_underlying(direction);
}

class uac2_driver final : public class_driver
{
public:
  explicit uac2_driver(std::shared_ptr<uac2_hub> hub) : hub_{std::move(hub)}
  {}

  uac2_driver(uac2_driver const&)                    = delete;
  auto operator=(uac2_driver const&) -> uac2_driver& = delete;
  uac2_driver(uac2_driver&&)                         = delete;
  auto operator=(uac2_driver&&) -> uac2_driver&      = delete;

  ~uac2_driver() override
  {
    if(channel_ == nullptr)
    {
      return;
    }
    channel_->interface.reset();
    channel_->ready = false;
    if(std::exchange(channel_->speaker.sending, false))
    {
      ++channel_->speaker.flushes;
    }
    channel_->speaker.running = false;
    channel_->mic.running     = false;
    for(auto const id : channel_->awaited)
    {
      channel_->results[id] = transfer_status::cancelled;
    }
    channel_->awaited.clear();
  }

  void configured(host_context& ctx) override
  {
    auto const& configuration = ctx.configuration();
    s_.interface              = ctx.interfaces().front();
    auto const functions      = uac2_functions(configuration);
    s_.function =
        static_cast<std::uint64_t>(std::ranges::find(functions, s_.interface) - functions.begin());
    bind();
    auto const* const control = configuration.find(s_.interface);
    auto const        found   = entities_of(control->extra);
    for(auto const number : ctx.interfaces() | std::views::drop(1))
    {
      plan_interface(configuration, found, number);
    }
    plan_mixer(control->extra, found);
    s_.steps.push_back({.kind = step_kind::probe_done});
    publish();
    advance(ctx);
  }

  void control_done(host_context& ctx, transfer_result const& result) override
  {
    if(not s_.busy or result.id != s_.request)
    {
      return;
    }
    auto const done = s_.current;
    s_.busy         = false;
    s_.request      = 0;
    if(std::exchange(s_.abandoned, false))
    {
      advance(ctx);
      return;
    }
    handle(ctx, done, result);
    publish();
    advance(ctx);
  }

  void transfer_done(host_context&, pipe_id pipe, transfer_result const& result) override
  {
    auto& speaker = s_.streams[direction_index(uac2_direction::speaker)];
    auto& mic     = s_.streams[direction_index(uac2_direction::mic)];
    if(mic.pipe != 0 and pipe == mic.pipe)
    {
      auto const& alt = s_.alts[mic.alt];
      uac2_frame  frame{.samples = {}, .at = result.at};
      if(result.status == transfer_status::completed and alt.subslot != 0)
      {
        for(std::size_t at = 0; at + alt.subslot <= result.data.size(); at += alt.subslot)
        {
          frame.samples.push_back(
              signed_of(le32(std::span{result.data}.subspan(at, alt.subslot), 0), alt.subslot));
        }
      }
      channel_->mic.received(std::move(frame));
    }
    else if(speaker.pipe != 0 and pipe == speaker.pipe and std::exchange(speaker.carried, false))
    {
      channel_->speaker.sent_at = result.at;
      channel_->speaker.sending = false;
    }
  }

  void frame(host_context& ctx, std::uint16_t) override
  {
    auto& speaker = s_.streams[direction_index(uac2_direction::speaker)];
    if(speaker.running and ctx.queued(speaker.pipe) == 0)
    {
      send(ctx, speaker);
    }
    auto& mic = s_.streams[direction_index(uac2_direction::mic)];
    if(mic.running
       and ctx.queued(mic.pipe) == 0
       and ctx.submit(mic.pipe, {.length = s_.alts[mic.alt].max_packet}) == 0)
    {
      halt(mic);
    }
  }

  void save(std::vector<std::byte>& out) const override
  {
    auto saved    = s_;
    saved.out     = samples_to_bytes(channel_->speaker.out);
    auto const at = channel_->speaker.sent_at;
    saved.sent_at = at ? std::optional{at->ps} : std::nullopt;
    reflex::serde::bson::serializer{out}.dump(saved);
  }

  void load(std::span<std::byte const> in) override
  {
    s_ = reflex::serde::bson::deserializer{in}.template load<driver_state>();
    bind();
    channel_->speaker.out     = bytes_to_samples(s_.out);
    channel_->speaker.sent_at = s_.sent_at ? std::optional{vtime{*s_.sent_at}} : std::nullopt;
    s_.out.clear();
    publish();
  }

  auto start(host_context& ctx, uac2_direction direction) -> std::uint64_t
  {
    auto& stream = s_.streams[direction_index(direction)];
    if(not stream.present or stream.running or stream.starting or stream.rate == 0)
    {
      return 0;
    }
    auto const& other = s_.streams[1 - direction_index(direction)];
    if(other.running
       and s_.alts[other.alt].clock == s_.alts[stream.alt].clock
       and other.rate != stream.rate)
    {
      return 0;
    }
    auto const operation = channel_->next_operation++;
    channel_->awaited.insert(operation);
    stream.starting  = true;
    stream.operation = operation;
    s_.steps.push_back(
        {.kind      = step_kind::start_begin,
         .alt       = stream.alt,
         .direction = static_cast<std::uint8_t>(direction),
         .operation = operation});
    advance(ctx);
    return operation;
  }

  void stop(host_context& ctx, uac2_direction direction)
  {
    auto& stream = s_.streams[direction_index(direction)];
    if(not stream.running and not stream.starting)
    {
      return;
    }
    auto on_bus = stream.running;
    if(stream.starting)
    {
      auto const operation = stream.operation;
      std::erase_if(s_.steps, [&](step const& queued) { return queued.operation == operation; });
      if(s_.busy and s_.current.operation == operation)
      {
        s_.abandoned = true;
        on_bus       = on_bus or s_.current.kind == step_kind::start_interface;
      }
      channel_->settle(operation, transfer_status::cancelled);
      stream.starting = false;
    }
    if(stream.pipe != 0)
    {
      ctx.close_pipe(stream.pipe);
      stream.pipe = 0;
    }
    stream.running = false;
    stream.carried = false;
    if(on_bus)
    {
      ctx.control(interface_request(s_.alts[stream.alt].interface, 0));
    }
    publish();
  }

  auto set_feature(
      host_context& ctx,
      std::uint8_t  unit,
      std::uint8_t  channel,
      uac2_control  control,
      std::int32_t  value) -> std::uint64_t
  {
    auto const found = std::ranges::find(s_.units, unit, &unit_state::id);
    if(found == s_.units.end() or channel > found->channels)
    {
      return 0;
    }
    auto const code = std::to_underlying(control);
    if(not writable(found->controls[channel], code))
    {
      return 0;
    }
    auto const operation = channel_->next_operation++;
    channel_->awaited.insert(operation);
    s_.steps.push_back(
        {.kind      = step_kind::set_feature,
         .unit      = static_cast<std::uint8_t>(found - s_.units.begin()),
         .control   = code,
         .channel   = channel,
         .value     = value,
         .operation = operation});
    advance(ctx);
    return operation;
  }

private:
  void bind()
  {
    channel_            = &hub_->channel(static_cast<std::size_t>(s_.function));
    channel_->interface = s_.interface;
  }

  void plan_interface(
      configuration_info const&             configuration,
      std::map<std::uint8_t, entity> const& found,
      std::uint8_t                          number)
  {
    auto const* const zero = configuration.find(number);
    if(zero == nullptr or zero->class_code != audio_class or zero->subclass != audio_streaming)
    {
      return;
    }
    for(auto const& setting : configuration.interfaces)
    {
      if(setting.number != number)
      {
        continue;
      }
      if(auto alt = candidate(setting, found))
      {
        auto const index = static_cast<std::uint32_t>(s_.alts.size());
        auto const direction =
            (alt->endpoint & 0x80u) != 0 ? uac2_direction::mic : uac2_direction::speaker;
        auto& stream = s_.streams[direction_index(direction)];
        if(not stream.present)
        {
          stream.present = true;
          stream.alt     = index;
        }
        s_.alts.push_back(std::move(*alt));
        s_.steps.push_back({.kind = step_kind::range_head, .alt = index});
      }
    }
    s_.steps.push_back(
        {.kind = step_kind::set_interface, .alt = no_alt, .interface = number, .alternate = 0});
  }

  static auto candidate(interface_info const& setting, std::map<std::uint8_t, entity> const& found)
      -> std::optional<alt_state>
  {
    if(setting.class_code != audio_class
       or setting.subclass != audio_streaming
       or setting.protocol != uac2_protocol
       or setting.endpoints.empty())
    {
      return std::nullopt;
    }
    auto const& endpoint = setting.endpoints.front();
    if(endpoint.max_packet == 0
       or endpoint.type != transfer_type::isochronous
       or endpoint.interval > 1)
    {
      return std::nullopt;
    }
    auto const general = find_descriptor(setting.extra, as_general, general_length);
    auto const format  = find_descriptor(setting.extra, format_type, format_length);
    if(not general or not format)
    {
      return std::nullopt;
    }
    auto const g = *general;
    auto const f = *format;
    if(g[5] != format_type_i
       or (le32(g, 6) & pcm) == 0
       or g[10] == 0
       or f[3] != format_type_i
       or f[4] == 0
       or f[4] > 4)
    {
      return std::nullopt;
    }
    auto const terminal = found.find(g[3]);
    if(terminal == found.end())
    {
      return std::nullopt;
    }
    auto const&                bytes = terminal->second.bytes;
    std::optional<clock_found> clock{};
    if(terminal->second.subtype == input_terminal and bytes.size() >= input_length)
    {
      clock = clock_of(found, bytes[7]);
    }
    else if(terminal->second.subtype == output_terminal and bytes.size() >= output_length)
    {
      clock = clock_of(found, bytes[8]);
    }
    if(not clock)
    {
      return std::nullopt;
    }
    return alt_state{
        .interface      = setting.number,
        .alternate      = setting.alternate,
        .endpoint       = endpoint.address,
        .max_packet     = endpoint.max_packet,
        .channels       = g[10],
        .subslot        = f[4],
        .bits           = f[5],
        .clock          = clock->id,
        .clock_controls = clock->controls,
    };
  }

  void plan_mixer(std::span<std::uint8_t const> extra, std::map<std::uint8_t, entity> const& found)
  {
    std::bitset<256> visited{};
    for(auto const& terminal : output_terminals(extra))
    {
      visited.set(terminal[3]);
      walk(found, terminal[7], visited);
      walk(found, terminal[8], visited);
    }
  }

  auto walk(std::map<std::uint8_t, entity> const& found, std::uint8_t id, std::bitset<256>& visited)
      -> bool
  {
    if(visited.test(id))
    {
      return true;
    }
    visited.set(id);
    auto const unit = found.find(id);
    if(unit == found.end())
    {
      return false;
    }
    auto const& bytes = unit->second.bytes;
    auto        pins  = [&](std::size_t count_at) {
      if(count_at >= bytes.size())
      {
        return;
      }
      for(std::size_t pin = 0; pin < bytes[count_at] and count_at + 1 + pin < bytes.size(); ++pin)
      {
        walk(found, bytes[count_at + 1 + pin], visited);
      }
    };
    switch(unit->second.subtype)
    {
      case feature_unit:
        if(bytes.size() < feature_head + control_bytes or not walk(found, bytes[4], visited))
        {
          return false;
        }
        plan_feature(bytes);
        break;
      case mixer_unit:
      case selector_unit:
        pins(4);
        break;
      case processing_unit:
      case extension_unit:
        pins(6);
        break;
      default:
        break;
    }
    return true;
  }

  void plan_feature(std::span<std::uint8_t const> bytes)
  {
    auto const channels = std::min((bytes.size() - feature_head) / control_bytes - 1, max_channels);
    unit_state unit{.id = bytes[3], .channels = static_cast<std::uint8_t>(channels)};
    for(std::size_t channel = 0; channel <= channels; ++channel)
    {
      unit.controls.push_back(le32(bytes, 5 + control_bytes * channel));
    }
    unit.mute.resize(channels + 1);
    unit.volume.resize(channels + 1);
    unit.range_min.resize(channels + 1);
    unit.range_max.resize(channels + 1);
    unit.range_res.resize(channels + 1);
    auto const index = static_cast<std::uint8_t>(s_.units.size());
    for(auto const control :
        {std::to_underlying(uac2_control::mute), std::to_underlying(uac2_control::volume)})
    {
      std::uint32_t mask{};
      std::uint32_t readonly{};
      for(std::size_t channel = 1; channel <= channels; ++channel)
      {
        if(readable(unit.controls[channel], control))
        {
          mask |= std::uint32_t{1} << (channel - 1);
          if(not writable(unit.controls[channel], control))
          {
            readonly |= std::uint32_t{1} << (channel - 1);
          }
        }
      }
      if((mask & 1u) != 0)
      {
        s_.steps.push_back(
            {.kind     = step_kind::control_begin,
             .unit     = index,
             .control  = control,
             .mask     = mask,
             .readonly = readonly});
      }
      if(readable(unit.controls[0], control))
      {
        s_.steps.push_back(
            {.kind     = step_kind::control_begin,
             .unit     = index,
             .control  = control,
             .mask     = 0,
             .readonly = writable(unit.controls[0], control) ? 0u : 1u});
      }
    }
    s_.units.push_back(std::move(unit));
  }

  [[nodiscard]] static auto interface_request(std::uint8_t interface, std::uint8_t alternate)
      -> setup_packet
  {
    return {
        .request_type = standard_interface,
        .request      = set_interface_code,
        .value        = alternate,
        .index        = interface,
        .length       = 0};
  }

  [[nodiscard]] auto clock_request(
      std::uint8_t  request,
      std::uint8_t  selector,
      std::uint8_t  clock,
      std::uint16_t length) const -> setup_packet
  {
    return {
        .request_type = class_in,
        .request      = request,
        .value        = static_cast<std::uint16_t>(selector << 8),
        .index        = static_cast<std::uint16_t>(clock << 8 | s_.interface),
        .length       = length};
  }

  [[nodiscard]] auto unit_request(
      std::uint8_t  request_type,
      std::uint8_t  request,
      step const&   at,
      std::uint8_t  channel,
      std::uint16_t length) const -> setup_packet
  {
    return {
        .request_type = request_type,
        .request      = request,
        .value        = static_cast<std::uint16_t>(at.control << 8 | channel),
        .index        = static_cast<std::uint16_t>(s_.units[at.unit].id << 8 | s_.interface),
        .length       = length};
  }

  [[nodiscard]] static auto min_channel(std::uint32_t mask) -> std::uint8_t
  {
    return mask == 0 ? 0 : static_cast<std::uint8_t>(std::countr_zero(mask) + 1);
  }

  [[nodiscard]] auto is_readonly(std::uint8_t channel) const -> bool
  {
    auto const& control = s_.control;
    return channel == 0 ? (control.mask == 0 and control.readonly != 0)
                        : (control.readonly & (std::uint32_t{1} << (channel - 1))) != 0;
  }

  static auto encode(std::int32_t value, std::size_t size) -> std::vector<std::uint8_t>
  {
    std::vector<std::uint8_t> out;
    auto const                raw = static_cast<std::uint32_t>(value);
    for(std::size_t i = 0; i < size; ++i)
    {
      out.push_back(static_cast<std::uint8_t>(raw >> (8 * i)));
    }
    return out;
  }

  void advance(host_context& ctx)
  {
    while(not s_.busy and not s_.steps.empty())
    {
      auto next = s_.steps.front();
      s_.steps.erase(s_.steps.begin());
      if(auto request = request_for(next))
      {
        s_.current = next;
        s_.busy    = true;
        s_.request = ctx.control(request->first, std::move(request->second));
      }
    }
    publish();
  }

  void push_front(std::initializer_list<step> steps)
  {
    s_.steps.insert(s_.steps.begin(), steps);
  }

  auto request_for(step const& at)
      -> std::optional<std::pair<setup_packet, std::vector<std::uint8_t>>>
  {
    auto const plain = [](setup_packet setup) {
      return std::optional{
          std::pair{setup, std::vector<std::uint8_t>{}}
      };
    };
    switch(at.kind)
    {
      case step_kind::range_head:
        return plain(clock_request(request_range, sampling_frequency, s_.alts[at.alt].clock, 2));
      case step_kind::range:
        return plain(clock_request(
            request_range, sampling_frequency, s_.alts[at.alt].clock,
            static_cast<std::uint16_t>(2 + subrange_size * static_cast<std::size_t>(at.value))));
      case step_kind::set_interface:
        return plain(interface_request(at.interface, at.alternate));
      case step_kind::clock_valid:
      case step_kind::clock_valid_after:
      {
        auto const& alt = s_.alts[at.alt];
        if(not readable(alt.clock_controls, clock_validity))
        {
          return std::nullopt;
        }
        return plain(clock_request(request_cur, clock_validity, alt.clock, 1));
      }
      case step_kind::get_rate:
      case step_kind::get_rate_after:
        return plain(clock_request(request_cur, sampling_frequency, s_.alts[at.alt].clock, 4));
      case step_kind::set_rate:
      {
        auto const& alt = s_.alts[at.alt];
        if(not writable(alt.clock_controls, sampling_frequency)
           or alt.current == static_cast<std::uint32_t>(at.value))
        {
          return std::nullopt;
        }
        auto setup         = clock_request(request_cur, sampling_frequency, alt.clock, 4);
        setup.request_type = class_out;
        return std::optional{
            std::pair{setup, encode(at.value, 4)}
        };
      }
      case step_kind::control_begin:
        begin_control(at);
        return std::nullopt;
      case step_kind::control_range:
        return plain(unit_request(
            class_in, request_range, at, min_channel(s_.control.mask),
            static_cast<std::uint16_t>(2 + 3 * value_size(at.control))));
      case step_kind::resolution_save:
      case step_kind::resolution_check:
      case step_kind::control_init:
        if(at.kind == step_kind::control_init
           and (s_.control.cached & (std::uint64_t{1} << at.channel)) != 0)
        {
          return std::nullopt;
        }
        return plain(unit_request(class_in, request_cur, at, at.channel, value_size(at.control)));
      case step_kind::resolution_test:
      {
        auto& control = s_.control;
        control.test =
            control.saved < control.max ? control.saved + control.res : control.saved - control.res;
        if(control.test < control.min or control.test > control.max)
        {
          control.res = control.last_valid;
          push_front({
              {.kind    = step_kind::resolution_restore,
               .unit    = at.unit,
               .control = at.control,
               .channel = at.channel}
          });
          return std::nullopt;
        }
        if(is_readonly(at.channel))
        {
          push_front({
              {.kind    = step_kind::resolution_check,
               .unit    = at.unit,
               .control = at.control,
               .channel = at.channel}
          });
          return std::nullopt;
        }
        return std::optional{
            std::pair{
                      unit_request(class_out, request_cur, at, at.channel, value_size(at.control)),
                      encode(control.test, value_size(at.control))}
        };
      }
      case step_kind::resolution_restore:
      case step_kind::control_set:
      {
        if(is_readonly(at.channel))
        {
          if(at.kind == step_kind::resolution_restore)
          {
            record_range();
            plan_init(at);
          }
          return std::nullopt;
        }
        auto const value = at.kind == step_kind::resolution_restore ? s_.control.saved : at.value;
        return std::optional{
            std::pair{
                      unit_request(class_out, request_cur, at, at.channel, value_size(at.control)),
                      encode(value, value_size(at.control))}
        };
      }
      case step_kind::set_feature:
        return std::optional{
            std::pair{
                      unit_request(class_out, request_cur, at, at.channel, value_size(at.control)),
                      encode(at.value, value_size(at.control))}
        };
      case step_kind::probe_done:
        s_.ready = true;
        return std::nullopt;
      case step_kind::start_begin:
      {
        auto const& stream = s_.streams[at.direction];
        auto const& other  = s_.streams[1 - at.direction];
        auto const  shared = other.running and s_.alts[other.alt].clock == s_.alts[at.alt].clock;
        step const  base{.alt = at.alt, .direction = at.direction, .operation = at.operation};
        auto        with = [&](step_kind kind) {
          auto made = base;
          made.kind = kind;
          return made;
        };
        auto rate  = with(step_kind::set_rate);
        rate.value = static_cast<std::int32_t>(stream.rate);
        if(shared)
        {
          push_front({with(step_kind::start_interface)});
        }
        else
        {
          push_front(
              {with(step_kind::clock_valid), with(step_kind::get_rate), rate,
               with(step_kind::clock_valid_after), with(step_kind::start_interface)});
        }
        return std::nullopt;
      }
      case step_kind::start_interface:
      {
        auto const& alt = s_.alts[at.alt];
        return plain(interface_request(alt.interface, alt.alternate));
      }
    }
    return std::nullopt;
  }

  void begin_control(step const& at)
  {
    s_.control = {
        .unit     = at.unit,
        .control  = at.control,
        .mask     = at.mask,
        .readonly = at.readonly,
        .min      = 0,
        .max      = 1,
        .res      = 1,
    };
    if(at.control == std::to_underlying(uac2_control::mute))
    {
      plan_init(at);
      return;
    }
    push_front({
        {.kind = step_kind::control_range, .unit = at.unit, .control = at.control, .field = 0}
    });
  }

  void plan_init(step const& at)
  {
    std::vector<step> init;
    for(auto const channel : channels_of(s_.control.mask))
    {
      init.push_back(
          {.kind    = step_kind::control_init,
           .unit    = at.unit,
           .control = at.control,
           .channel = channel});
    }
    s_.steps.insert(s_.steps.begin(), init.begin(), init.end());
  }

  void
      store(std::uint8_t unit_index, std::uint8_t control, std::uint8_t channel, std::int32_t value)
  {
    auto&      unit = s_.units[unit_index];
    auto const bit  = std::uint64_t{1} << channel;
    if(channel > unit.channels)
    {
      return;
    }
    if(control == std::to_underlying(uac2_control::mute))
    {
      unit.mute[channel] = value;
      unit.mute_read |= bit;
    }
    else
    {
      unit.volume[channel] = value;
      unit.volume_read |= bit;
    }
  }

  void update_rate(std::uint32_t alt_index)
  {
    for(auto& stream : s_.streams)
    {
      if(not stream.present)
      {
        continue;
      }
      auto const& chosen = s_.alts[stream.alt];
      auto const& alt    = s_.alts[alt_index];
      if(alt.usable
         and not chosen.usable
         and alt.interface == chosen.interface and alt_index > stream.alt)
      {
        stream.alt = alt_index;
      }
      if(stream.alt != alt_index)
      {
        continue;
      }
      auto const highest = alt.rates.empty() ? 0u : std::ranges::max(alt.rates);
      stream.rate        = alt.current != 0 ? alt.current : highest;
    }
  }

  void clock_rate(std::uint32_t alt_index, std::uint32_t rate)
  {
    auto const clock = s_.alts[alt_index].clock;
    for(std::uint32_t index = 0; index < s_.alts.size(); ++index)
    {
      if(s_.alts[index].clock == clock)
      {
        s_.alts[index].current = rate;
        update_rate(index);
      }
    }
  }

  void abort_start(step const& at, transfer_status status)
  {
    auto& stream    = s_.streams[at.direction];
    stream.starting = false;
    std::erase_if(s_.steps, [&](step const& queued) { return queued.operation == at.operation; });
    channel_->settle(
        at.operation, status == transfer_status::completed ? transfer_status::error : status);
  }

  void handle(host_context& ctx, step const& at, transfer_result const& result)
  {
    auto const ok   = result.status == transfer_status::completed;
    auto const data = std::span<std::uint8_t const>{result.data};
    switch(at.kind)
    {
      case step_kind::range_head:
        if(ok)
        {
          auto const count = std::min<std::size_t>(le16(data, 0), max_subranges);
          push_front({
              {.kind  = step_kind::range,
               .alt   = at.alt,
               .value = static_cast<std::int32_t>(count)}
          });
        }
        break;
      case step_kind::range:
        if(ok)
        {
          parse_rates(at.alt, data, static_cast<std::size_t>(at.value));
          auto const& alt = s_.alts[at.alt];
          auto const  top =
              alt.rates.empty() ? 0 : static_cast<std::int32_t>(std::ranges::max(alt.rates));
          step const base{.alt = at.alt};
          auto       with = [&](step_kind kind) {
            auto made = base;
            made.kind = kind;
            return made;
          };
          auto zero        = with(step_kind::set_interface);
          zero.interface   = alt.interface;
          auto rate        = with(step_kind::set_rate);
          rate.value       = top;
          auto select      = with(step_kind::set_interface);
          select.interface = alt.interface;
          select.alternate = alt.alternate;
          push_front(
              {zero, with(step_kind::clock_valid), with(step_kind::get_rate), rate,
               with(step_kind::clock_valid_after), select});
          update_rate(at.alt);
        }
        break;
      case step_kind::get_rate:
        if(ok and data.size() >= 4)
        {
          clock_rate(at.alt, le32(data, 0));
        }
        else
        {
          s_.alts[at.alt].current = 0;
          update_rate(at.alt);
        }
        break;
      case step_kind::get_rate_after:
        if(ok and data.size() >= 4)
        {
          clock_rate(at.alt, le32(data, 0));
        }
        break;
      case step_kind::set_rate:
        if(ok)
        {
          auto after = at;
          after.kind = step_kind::get_rate_after;
          push_front({after});
        }
        else if(at.operation != 0)
        {
          abort_start(at, result.status);
        }
        else if(not s_.steps.empty() and s_.steps.front().kind == step_kind::clock_valid_after)
        {
          s_.steps.erase(s_.steps.begin());
        }
        break;
      case step_kind::clock_valid_after:
        if(at.operation != 0 and not(ok and not data.empty() and data[0] != 0))
        {
          abort_start(at, result.status);
        }
        break;
      case step_kind::control_range:
      {
        auto const size  = value_size(at.control);
        auto const slot  = std::array<std::size_t, 3>{1, 0, 2}.at(at.field);
        auto const field = std::size_t{2} + std::size_t{size} * slot;
        auto const value =
            signed_of(field + size <= data.size() ? le32(data.subspan(field, size), 0) : 0, size);
        auto& control = s_.control;
        if(at.field == 0)
        {
          if(not ok)
          {
            break;
          }
          control.max = value;
          push_front({
              {.kind    = step_kind::control_range,
               .unit    = at.unit,
               .control = at.control,
               .field   = 1}
          });
          break;
        }
        if(at.field == 1)
        {
          if(not ok)
          {
            break;
          }
          control.min = value;
          push_front({
              {.kind    = step_kind::control_range,
               .unit    = at.unit,
               .control = at.control,
               .field   = 2}
          });
          break;
        }
        control.res        = ok and value != 0 ? value : 1;
        control.last_valid = control.res;
        record_range();
        if(control.min + control.res < control.max)
        {
          push_front({
              {.kind    = step_kind::resolution_save,
               .unit    = at.unit,
               .control = at.control,
               .channel = min_channel(control.mask)}
          });
        }
        else
        {
          plan_init(at);
        }
        break;
      }
      case step_kind::resolution_save:
        if(ok)
        {
          s_.control.saved = signed_of(le32(data, 0), value_size(at.control));
          push_front({
              {.kind    = step_kind::resolution_test,
               .unit    = at.unit,
               .control = at.control,
               .channel = at.channel}
          });
        }
        else
        {
          plan_init(at);
        }
        break;
      case step_kind::resolution_test:
        if(ok)
        {
          cache(at, s_.control.test);
          push_front({
              {.kind    = step_kind::resolution_check,
               .unit    = at.unit,
               .control = at.control,
               .channel = at.channel}
          });
        }
        else if(retry(at, result.status))
        {
          break;
        }
        else
        {
          s_.control.res = s_.control.last_valid;
          push_front({
              {.kind    = step_kind::resolution_restore,
               .unit    = at.unit,
               .control = at.control,
               .channel = at.channel}
          });
        }
        break;
      case step_kind::resolution_check:
      {
        auto& control = s_.control;
        if(not ok)
        {
          control.res = control.last_valid;
        }
        else if(signed_of(le32(data, 0), value_size(at.control)) != control.test)
        {
          control.res *= 2;
          push_front({
              {.kind    = step_kind::resolution_test,
               .unit    = at.unit,
               .control = at.control,
               .channel = at.channel}
          });
          break;
        }
        push_front({
            {.kind    = step_kind::resolution_restore,
             .unit    = at.unit,
             .control = at.control,
             .channel = at.channel}
        });
        break;
      }
      case step_kind::resolution_restore:
        if(ok)
        {
          cache(at, s_.control.saved);
        }
        else if(retry(at, result.status))
        {
          break;
        }
        record_range();
        plan_init(at);
        break;
      case step_kind::control_init:
        if(ok)
        {
          cache(at, signed_of(le32(data, 0), value_size(at.control)));
        }
        else
        {
          push_front({
              {.kind    = step_kind::control_set,
               .unit    = at.unit,
               .control = at.control,
               .channel = at.channel,
               .value   = s_.control.min}
          });
        }
        break;
      case step_kind::control_set:
        if(ok)
        {
          cache(at, at.value);
        }
        else
        {
          retry(at, result.status);
        }
        break;
      case step_kind::set_feature:
        if(ok)
        {
          store(at.unit, at.control, at.channel, at.value);
        }
        else if(retry(at, result.status))
        {
          break;
        }
        channel_->settle(at.operation, result.status);
        break;
      case step_kind::start_interface:
      {
        auto& stream    = s_.streams[at.direction];
        stream.starting = false;
        if(not ok)
        {
          channel_->settle(at.operation, result.status);
          break;
        }
        auto const& alt     = s_.alts[at.alt];
        auto const* setting = ctx.configuration().find(alt.interface, alt.alternate);
        if(setting == nullptr or setting->endpoints.empty())
        {
          channel_->settle(at.operation, transfer_status::error);
          break;
        }
        stream.pipe        = ctx.open_pipe(setting->endpoints.front());
        stream.running     = true;
        stream.accumulator = 0;
        channel_->settle(at.operation, transfer_status::completed);
        break;
      }
      default:
        break;
    }
  }

  auto retry(step const& at, transfer_status status) -> bool
  {
    if(status == transfer_status::timed_out or at.attempt + 1 >= mixer_attempts)
    {
      return false;
    }
    auto again = at;
    ++again.attempt;
    push_front({again});
    return true;
  }

  void cache(step const& at, std::int32_t value)
  {
    s_.control.cached |= std::uint64_t{1} << at.channel;
    store(at.unit, at.control, at.channel, value);
  }

  void record_range()
  {
    auto const& control = s_.control;
    if(control.control != std::to_underlying(uac2_control::volume))
    {
      return;
    }
    auto& unit = s_.units[control.unit];
    for(auto const channel : channels_of(control.mask))
    {
      if(channel > unit.channels)
      {
        continue;
      }
      unit.range_min[channel] = control.min;
      unit.range_max[channel] = control.max;
      unit.range_res[channel] = control.res;
      unit.range_read |= std::uint64_t{1} << channel;
    }
  }

  void parse_rates(std::uint32_t alt_index, std::span<std::uint8_t const> data, std::size_t count)
  {
    auto& alt  = s_.alts[alt_index];
    alt.usable = true;
    alt.rates.clear();
    for(std::size_t i = 0; i < count; ++i)
    {
      auto const at  = 2 + subrange_size * i;
      auto const min = static_cast<std::int32_t>(le32(data, at));
      auto const max = static_cast<std::int32_t>(le32(data, at + 4));
      auto const res = static_cast<std::int32_t>(le32(data, at + 8));
      if(max < 0 or min < 0 or res < 0 or max < min)
      {
        continue;
      }
      if(res == 1)
      {
        alt.rates = {static_cast<std::uint32_t>(min), static_cast<std::uint32_t>(max)};
        return;
      }
      for(auto rate = static_cast<std::int64_t>(min); rate <= max; rate += res)
      {
        alt.rates.push_back(static_cast<std::uint32_t>(rate));
        if(alt.rates.size() >= max_rates or res == 0)
        {
          break;
        }
      }
      if(alt.rates.size() >= max_rates)
      {
        return;
      }
    }
  }

  void send(host_context& ctx, stream_state& stream)
  {
    auto const& alt   = s_.alts[stream.alt];
    auto&       queue = channel_->speaker.out;
    stream.accumulator += stream.rate % frames_per_second;
    auto samples = stream.rate / frames_per_second;
    if(stream.accumulator >= frames_per_second)
    {
      stream.accumulator -= frames_per_second;
      ++samples;
    }
    auto const frame_bytes = std::size_t{alt.channels} * alt.subslot;
    auto const values = std::min<std::size_t>(samples, alt.max_packet / frame_bytes) * alt.channels;
    std::vector<std::uint8_t> packet;
    packet.reserve(values * alt.subslot);
    stream.carried            = not queue.empty();
    channel_->speaker.sending = stream.carried;
    for(std::size_t i = 0; i < values; ++i)
    {
      std::int32_t sample{};
      if(not queue.empty())
      {
        sample = queue.front();
        queue.pop_front();
      }
      auto const raw = static_cast<std::uint32_t>(sample);
      for(std::size_t byte = 0; byte < alt.subslot; ++byte)
      {
        packet.push_back(static_cast<std::uint8_t>(raw >> (8 * byte)));
      }
    }
    if(ctx.submit(stream.pipe, {.data = std::move(packet), .length = 0}) == 0)
    {
      halt(stream);
    }
  }

  void halt(stream_state& stream)
  {
    stream.running = false;
    stream.carried = false;
    stream.pipe    = 0;
    publish();
  }

  void publish()
  {
    channel_->ready = s_.ready;
    for(auto const direction : {uac2_direction::speaker, uac2_direction::mic})
    {
      auto const& state  = s_.streams[direction_index(direction)];
      auto&       stream = channel_->stream(direction);
      if(not state.present)
      {
        stream.interface.reset();
        stream.running = false;
        stream.sending = false;
        continue;
      }
      auto const& alt   = s_.alts[state.alt];
      stream.interface  = alt.interface;
      stream.alternate  = alt.alternate;
      stream.endpoint   = alt.endpoint;
      stream.max_packet = alt.max_packet;
      stream.format     = {.channels = alt.channels, .subslot = alt.subslot, .bits = alt.bits};
      stream.clock      = alt.clock;
      stream.rates      = alt.rates;
      stream.rate       = state.rate;
      stream.running    = state.running;
      stream.sending    = state.carried;
    }
    channel_->units.clear();
    for(auto const& unit : s_.units)
    {
      uac2_feature_unit shown{.id = unit.id, .channels = unit.channels, .controls = unit.controls};
      for(std::size_t channel = 0; channel <= unit.channels; ++channel)
      {
        auto const bit = std::uint64_t{1} << channel;
        shown.mute.push_back(
            (unit.mute_read & bit) != 0 ? std::optional{unit.mute[channel] != 0} : std::nullopt);
        shown.volume.push_back(
            (unit.volume_read & bit) != 0
                ? std::optional{static_cast<std::int16_t>(unit.volume[channel])}
                : std::nullopt);
        shown.volume_range.push_back(
            (unit.range_read & bit) != 0
                ? std::optional{uac2_range{.min = unit.range_min[channel], .max = unit.range_max[channel], .res = unit.range_res[channel]}}
                : std::nullopt);
      }
      channel_->units.push_back(std::move(shown));
    }
  }

  static constexpr std::uint32_t no_alt = 0xffff'ffff;

  std::shared_ptr<uac2_hub> hub_;
  driver_state              s_{};
  uac2_channel*             channel_{};
};

struct unbound_channel
{
  std::uint64_t          function{};
  std::vector<std::byte> samples{};
};

struct hub_state
{
  std::vector<unbound_channel> unbound{};
};

auto bound_driver_of(host& owner, uac2_channel const& channel)
    -> std::optional<std::pair<uac2_driver*, std::size_t>>
{
  if(not channel.interface)
  {
    return std::nullopt;
  }
  auto const drivers = owner.drivers();
  for(std::size_t index = 0; index < drivers.size(); ++index)
  {
    auto const& bound = drivers[index];
    if(bound.name == uac2_name
       and not bound.interfaces.empty()
       and bound.interfaces.front() == *channel.interface)
    {
      if(auto* const driver = dynamic_cast<uac2_driver*>(bound.driver.get()))
      {
        return std::pair{driver, index};
      }
    }
  }
  return std::nullopt;
}
}

void uac2_stream::received(uac2_frame frame)
{
  in.push_back(std::move(frame));
  while(in.size() > limit)
  {
    in.pop_front();
    ++dropped;
  }
}

auto uac2_stream::take(std::uint64_t count) -> std::vector<uac2_frame>
{
  std::vector<uac2_frame> out;
  while(count-- != 0 and not in.empty())
  {
    out.push_back(std::move(in.front()));
    in.pop_front();
  }
  return out;
}

void uac2_stream::set_limit(std::uint64_t count)
{
  limit = count;
  while(in.size() > limit)
  {
    in.pop_front();
    ++dropped;
  }
}

auto uac2_channel::stream(uac2_direction direction) -> uac2_stream&
{
  return direction == uac2_direction::speaker ? speaker : mic;
}

void uac2_channel::settle(std::uint64_t operation, transfer_status status)
{
  if(awaited.erase(operation) != 0)
  {
    results[operation] = status;
  }
}

void uac2_channel::discard()
{
  ready = false;
  for(auto* const stream : {&speaker, &mic})
  {
    auto const limit = stream->limit;
    *stream          = {};
    stream->limit    = limit;
  }
  units.clear();
  awaited.clear();
  results.clear();
}

auto uac2_hub::channel(std::size_t function) -> uac2_channel&
{
  return channels_[function];
}

void uac2_hub::save(std::vector<std::byte>& out) const
{
  hub_state saved;
  for(auto const& [function, channel] : channels_)
  {
    if(not channel.interface and not channel.speaker.out.empty())
    {
      saved.unbound.push_back(
          {.function = function, .samples = samples_to_bytes(channel.speaker.out)});
    }
  }
  reflex::serde::bson::serializer{out}.dump(saved);
}

void uac2_hub::load(std::span<std::byte const> in)
{
  for(auto& channel : channels_ | std::views::values)
  {
    channel.speaker.out.clear();
  }
  if(in.empty())
  {
    return;
  }
  for(auto const& pending :
      reflex::serde::bson::deserializer{in}.template load<hub_state>().unbound)
  {
    channel(static_cast<std::size_t>(pending.function)).speaker.out =
        bytes_to_samples(pending.samples);
  }
}

void uac2_hub::discard()
{
  for(auto& channel : channels_ | std::views::values)
  {
    channel.discard();
  }
}

auto uac2_claim(configuration_info const& configuration, interface_info const& interface)
    -> std::vector<std::uint8_t>
{
  if(interface.alternate != 0
     or interface.class_code != audio_class
     or interface.subclass != audio_control
     or interface.protocol != uac2_protocol)
  {
    return {};
  }
  auto members = configuration.function_of(interface.number);
  if(members.size() < 2)
  {
    return {};
  }
  std::erase(members, interface.number);
  members.insert(members.begin(), interface.number);
  return members;
}

auto uac2_functions(configuration_info const& configuration) -> std::vector<std::uint8_t>
{
  return configuration.interfaces
       | std::views::filter([&](interface_info const& setting) {
           return not uac2_claim(configuration, setting).empty();
         })
       | std::views::transform(&interface_info::number)
       | std::ranges::to<std::vector>();
}

auto uac2_start(host& owner, uac2_hub& hub, std::size_t function, uac2_direction direction)
    -> std::uint64_t
{
  auto const found = bound_driver_of(owner, hub.channel(function));
  if(not found)
  {
    return 0;
  }
  auto ctx = owner.context(found->second);
  return found->first->start(ctx, direction);
}

void uac2_stop(host& owner, uac2_hub& hub, std::size_t function, uac2_direction direction)
{
  auto&      channel = hub.channel(function);
  auto const dropped = direction == uac2_direction::speaker
                   and (channel.speaker.sending or not channel.speaker.out.empty());
  if(auto const found = bound_driver_of(owner, channel))
  {
    auto ctx = owner.context(found->second);
    found->first->stop(ctx, direction);
  }
  if(dropped)
  {
    channel.speaker.out.clear();
    ++channel.speaker.flushes;
  }
}

auto uac2_set_feature(
    host&        owner,
    uac2_hub&    hub,
    std::size_t  function,
    std::uint8_t unit,
    std::uint8_t channel,
    uac2_control control,
    std::int32_t value) -> std::uint64_t
{
  auto const found = bound_driver_of(owner, hub.channel(function));
  if(not found)
  {
    return 0;
  }
  auto ctx = owner.context(found->second);
  return found->first->set_feature(ctx, unit, channel, control, value);
}

auto uac2_factory(std::shared_ptr<uac2_hub> hub) -> driver_factory
{
  return {
      .name  = std::string{uac2_name},
      .claim = uac2_claim,
      .make  = [hub]() -> std::unique_ptr<class_driver> {
        return std::make_unique<uac2_driver>(hub);
      },
      .save = [hub](std::vector<std::byte>& out) { hub->save(out); },
      .load = [hub](std::span<std::byte const> in) { hub->load(in); },
  };
}
}
