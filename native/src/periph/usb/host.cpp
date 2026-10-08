#include <popcorn/periph/usb_host.hpp>

#include <reflex/enum.hpp>
#include <reflex/serde/bson.hpp>

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <format>
#include <iterator>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace popcorn::periph::usb
{
namespace
{
constexpr std::uint64_t ps_per_ns           = 1000;
constexpr std::uint64_t frame_ps            = 1'000'000'000;
constexpr std::uint64_t sof_ps              = 3'000'000;
constexpr std::uint64_t non_iso_overhead_ps = 9'107'000;
constexpr std::uint64_t iso_in_overhead_ps  = 7'268'000;
constexpr std::uint64_t iso_out_overhead_ps = 6'265'000;
constexpr std::uint64_t bit_ps              = 83'540;
constexpr std::uint64_t fixed_millibits     = 3'167;
constexpr std::uint64_t stuffed_millibits   = 9'334;
constexpr std::uint8_t  error_limit         = 3;
constexpr std::uint16_t frame_mask          = 0x7ff;
constexpr pipe_id       default_pipe        = 0;
constexpr std::uint16_t first_packet0       = 64;
constexpr std::uint16_t device_length       = 18;
constexpr std::uint16_t configuration_head  = 9;
constexpr std::uint16_t string_length       = 255;

/**
 * @brief Pause after a pass in which every busy control and bulk pipe NAKed, before the next
 *        pass in the same frame. Pacing borrowed from EHCI 1.0 4.8.4.1 AsyncSchedSleepTime,
 *        10 us. Real hosts at RL 0, Linux's setting for full speed behind a TT, retry faster.
 *        The 10 us gap keeps the wake-up count bounded.
 */
constexpr std::uint64_t async_sleep_ps = 10'000'000;
/**
 * @brief Frames from the one a pipe opens in to its first isochronous packet: the earliest
 *        start xHCI 1.2 4.11.2.5 allows, current frame + IST rounded up to a frame + 1, and the
 *        lower bound of Linux's xhci_queue_isoc_tx_prepare(), MFINDEX + IST + XHCI_CFC_DELAY
 *        10 microframes rounded up to a frame.
 */
constexpr std::uint64_t iso_start_frames = 2;

constexpr std::uint8_t device_to_host            = 0x80;
constexpr std::uint8_t get_descriptor            = 6;
constexpr std::uint8_t set_address_request       = 5;
constexpr std::uint8_t set_configuration_request = 9;
constexpr std::uint8_t device_type               = 1;
constexpr std::uint8_t configuration_type        = 2;
constexpr std::uint8_t string_type               = 3;
constexpr std::uint8_t interface_type            = 4;
constexpr std::uint8_t endpoint_type             = 5;
constexpr std::uint8_t association_type          = 11;
constexpr std::size_t  interface_length          = 9;
constexpr std::size_t  endpoint_length           = 7;
constexpr std::size_t  association_length        = 8;
constexpr std::size_t  configuration_length      = 9;
constexpr std::size_t  max_packet0_offset        = 7;
constexpr std::uint8_t max_interval_exponent     = 16;

enum class origin : std::uint8_t
{
  enumeration,
  driver,
  user,
};

enum class stage : std::uint8_t
{
  setup,
  data,
  status,
};

struct transfer_state
{
  transfer_id                  id{};
  origin                       source{};
  std::uint32_t                owner{};
  setup_packet                 setup{};
  std::vector<std::byte>       data{};
  std::uint32_t                length{};
  std::uint32_t                offset{};
  bool                         zero_packet{};
  bool                         zlp_sent{};
  bool                         logged{};
  stage                        step{};
  std::uint8_t                 errors{};
  std::optional<std::uint64_t> started_ps{};
};

struct pipe_state
{
  pipe_id                     id{};
  std::uint32_t               owner{};
  std::uint8_t                endpoint{};
  transfer_type               type{};
  std::uint16_t               max_packet{};
  std::uint32_t               period{1};
  data_pid                    toggle{};
  std::uint64_t               next_poll{};
  bool                        overran{};
  std::vector<transfer_state> queue{};
};

struct driver_record
{
  std::string               name{};
  std::vector<std::uint8_t> interfaces{};
  std::vector<std::byte>    bytes{};
};

struct factory_record
{
  std::string            name{};
  std::vector<std::byte> bytes{};
};

struct host_state
{
  device_state                      device{};
  std::uint8_t                      address{};
  std::uint8_t                      configuration{};
  std::uint16_t                     language{};
  std::uint64_t                     now_ps{};
  std::optional<std::uint64_t>      debounce_end{};
  std::optional<std::uint64_t>      reset_end{};
  std::optional<std::uint64_t>      hold_until{};
  std::optional<std::uint64_t>      next_frame{};
  std::optional<std::uint64_t>      next_slot{};
  std::uint64_t                     frame_index{};
  std::uint32_t                     step{};
  std::uint32_t                     string_cursor{};
  bool                              step_active{};
  bool                              enumerated{};
  std::optional<enumeration_action> failed{};
  std::vector<std::byte>            device_bytes{};
  std::vector<std::byte>            configuration_bytes{};
  std::vector<string_descriptor>    strings{};
  std::vector<std::uint8_t>         string_indices{};
  std::vector<pipe_state>           pipes{};
  pipe_id                           next_pipe{default_pipe + 1};
  transfer_id                       next_transfer{1};
  std::uint32_t                     nak_streak{};
  std::optional<pipe_id>            last_served{};
  std::vector<driver_record>        drivers{};
  std::vector<factory_record>       factories{};
};

struct outcome
{
  std::uint64_t                  time{};
  bool                           progress{};
  std::optional<transfer_status> done{};
};

auto to_ps(std::chrono::nanoseconds duration) -> std::uint64_t
{
  return static_cast<std::uint64_t>(duration.count()) * ps_per_ns;
}

auto le16(std::span<std::uint8_t const> bytes, std::size_t at) -> std::uint16_t
{
  return static_cast<std::uint16_t>(bytes[at] | (bytes[at + 1] << 8));
}

/** @brief Worst-case bus time of a full-speed transaction, the formulas of USB 2.0 5.11.3. */
auto bus_time(transfer_type type, bool in, std::size_t bytes) -> std::uint64_t
{
  auto const overhead = type != transfer_type::isochronous ? non_iso_overhead_ps
                      : in                                 ? iso_in_overhead_ps
                                                           : iso_out_overhead_ps;
  return overhead + bit_ps * (fixed_millibits + stuffed_millibits * bytes) / 1000;
}

auto flip(data_pid pid) -> data_pid
{
  return pid == data_pid::data0 ? data_pid::data1 : data_pid::data0;
}

auto is_in(setup_packet const& setup) -> bool
{
  return (setup.request_type & device_to_host) != 0;
}

auto descriptor_request(
    std::uint8_t  type,
    std::uint8_t  index,
    std::uint16_t language,
    std::uint16_t length) -> setup_packet
{
  return {
      .request_type = device_to_host,
      .request      = get_descriptor,
      .value        = static_cast<std::uint16_t>((type << 8) | index),
      .index        = language,
      .length       = length};
}

auto period_of(endpoint_info const& endpoint) -> std::uint32_t
{
  if(endpoint.type == transfer_type::isochronous)
  {
    auto const exponent = std::clamp<std::uint8_t>(endpoint.interval, 1, max_interval_exponent);
    return std::uint32_t{1} << (exponent - 1);
  }
  return std::max<std::uint32_t>(endpoint.interval, 1);
}

static_assert(std::same_as<std::uint8_t, unsigned char>);

auto octets(std::span<std::byte const> in) -> std::span<std::uint8_t const>
{
  return {reinterpret_cast<std::uint8_t const*>(in.data()), in.size()};
}

auto to_byte_vector(std::span<std::uint8_t const> in) -> std::vector<std::byte>
{
  return std::ranges::to<std::vector<std::byte>>(std::as_bytes(in));
}

auto copy_octets(std::span<std::byte const> in) -> std::vector<std::uint8_t>
{
  return std::ranges::to<std::vector<std::uint8_t>>(octets(in));
}
}

auto configuration_info::find(std::uint8_t number, std::uint8_t alternate) const
    -> interface_info const*
{
  auto const found = std::ranges::find_if(interfaces, [&](interface_info const& entry) {
    return entry.number == number and entry.alternate == alternate;
  });
  return found == interfaces.end() ? nullptr : &*found;
}

auto configuration_info::function_of(std::uint8_t number) const -> std::vector<std::uint8_t>
{
  for(auto const& association : associations)
  {
    auto const first = int{association.first_interface};
    auto const last  = first + association.count;
    if(number >= first and number < last)
    {
      return std::views::iota(first, last)
           | std::views::transform([](int n) { return static_cast<std::uint8_t>(n); })
           | std::ranges::to<std::vector>();
    }
  }
  return {number};
}

auto parse_device_descriptor(std::span<std::uint8_t const> bytes)
    -> std::optional<device_descriptor>
{
  if(bytes.size() < device_length or bytes[1] != device_type)
  {
    return std::nullopt;
  }
  return device_descriptor{
      .usb_version         = le16(bytes, 2),
      .class_code          = bytes[4],
      .subclass            = bytes[5],
      .protocol            = bytes[6],
      .max_packet0         = bytes[7],
      .vendor              = le16(bytes, 8),
      .product             = le16(bytes, 10),
      .device_version      = le16(bytes, 12),
      .manufacturer_string = bytes[14],
      .product_string      = bytes[15],
      .serial_string       = bytes[16],
      .configurations      = bytes[17]};
}

auto parse_configuration(std::span<std::uint8_t const> bytes)
    -> std::expected<configuration_info, std::string>
{
  if(bytes.size() < configuration_length or bytes[1] != configuration_type)
  {
    return std::unexpected{std::string{"not a configuration descriptor"}};
  }
  if(le16(bytes, 2) != bytes.size())
  {
    return std::unexpected{
        std::format("wTotalLength {} but {} bytes", le16(bytes, 2), bytes.size())};
  }
  configuration_info config{
      .value           = bytes[5],
      .attributes      = bytes[7],
      .max_power       = bytes[8],
      .string_index    = bytes[6],
      .interface_count = bytes[4]};
  interface_info* interface = nullptr;
  endpoint_info*  endpoint  = nullptr;
  for(std::size_t at = bytes[0]; at < bytes.size();)
  {
    auto const length = bytes[at];
    if(length < 2 or at + length > bytes.size())
    {
      return std::unexpected{std::format("descriptor at offset {} has length {}", at, length)};
    }
    auto const descriptor = bytes.subspan(at, length);
    auto const type       = descriptor[1];
    if(type == interface_type and length >= interface_length)
    {
      config.interfaces.push_back(
          {.number       = descriptor[2],
           .alternate    = descriptor[3],
           .class_code   = descriptor[5],
           .subclass     = descriptor[6],
           .protocol     = descriptor[7],
           .string_index = descriptor[8]});
      interface = &config.interfaces.back();
      endpoint  = nullptr;
    }
    else if(type == endpoint_type and length >= endpoint_length and interface != nullptr)
    {
      interface->endpoints.push_back(
          {.address    = descriptor[2],
           .type       = static_cast<transfer_type>(descriptor[3] & 3u),
           .attributes = descriptor[3],
           .max_packet = static_cast<std::uint16_t>(le16(descriptor, 4) & 0x7ffu),
           .interval   = descriptor[6]});
      endpoint = &interface->endpoints.back();
    }
    else if(type == association_type and length >= association_length)
    {
      config.associations.push_back(
          {.first_interface = descriptor[2],
           .count           = descriptor[3],
           .class_code      = descriptor[4],
           .subclass        = descriptor[5],
           .protocol        = descriptor[6],
           .string_index    = descriptor[7]});
    }
    else
    {
      auto& extra = endpoint != nullptr  ? endpoint->extra
                  : interface != nullptr ? interface->extra
                                         : config.extra;
      extra.insert(extra.end(), descriptor.begin(), descriptor.end());
    }
    at += length;
  }
  return config;
}

auto default_enumeration() -> std::vector<enumeration_step>
{
  using enum enumeration_action;
  return {
      {get_device_head},
      {bus_reset},
      {set_address},
      {wait, std::chrono::milliseconds{10}},
      {get_device},
      {get_configuration_head},
      {get_configuration},
      {get_languages},
      {get_strings},
      {set_configuration}
  };
}

void class_driver::control_done(host_context&, transfer_result const&)
{}

void class_driver::transfer_done(host_context&, pipe_id, transfer_result const&)
{}

void class_driver::frame(host_context&, std::uint16_t)
{}

void class_driver::save(std::vector<std::byte>&) const
{}

void class_driver::load(std::span<std::byte const>)
{}

struct host::impl
{
  host&                             self;
  device_port&                      port;
  host_options                      options;
  host_state                        s;
  std::optional<configuration_info> parsed{};
  std::vector<bound_driver>         drivers{};
  std::vector<transfer_result>      results{};
  std::vector<transfer_id>          forgotten{};
  std::uint64_t                     epoch = 0;
  std::vector<log_entry>            entries{};

  impl(host& owner, device_port& device, host_options chosen)
      : self{owner}, port{device}, options{std::move(chosen)}
  {}

  void warn(std::string const& message) const
  {
    if(options.warn and not deferred_warnings::hold(options.warn, message))
    {
      options.warn(message);
    }
  }

  [[nodiscard]] auto frame_number() const -> std::uint16_t
  {
    return static_cast<std::uint16_t>(s.frame_index & frame_mask);
  }

  [[nodiscard]] auto find_pipe(pipe_id id) -> pipe_state*
  {
    auto const found = std::ranges::find(s.pipes, id, &pipe_state::id);
    return found == s.pipes.end() ? nullptr : &*found;
  }

  [[nodiscard]] auto find_pipe(pipe_id id) const -> pipe_state const*
  {
    auto const found = std::ranges::find(s.pipes, id, &pipe_state::id);
    return found == s.pipes.end() ? nullptr : &*found;
  }

  [[nodiscard]] static auto non_periodic(pipe_state const& pipe) -> bool
  {
    return pipe.type == transfer_type::control or pipe.type == transfer_type::bulk;
  }

  [[nodiscard]] auto busy_pipes() const -> std::uint32_t
  {
    return static_cast<std::uint32_t>(std::ranges::count_if(s.pipes, [](pipe_state const& pipe) {
      return non_periodic(pipe) and not pipe.queue.empty();
    }));
  }

  [[nodiscard]] auto attached() const -> bool
  {
    return s.device != device_state::detached;
  }

  void schedule()
  {
    std::optional<std::uint64_t> next{};
    for(auto const& due : {s.debounce_end, s.reset_end, s.hold_until, s.next_frame, s.next_slot})
    {
      if(due and (not next or *due < *next))
      {
        next = due;
      }
    }
    if(next)
    {
      port.wake_at(vtime{*next});
    }
  }

  auto enqueue(pipe_id id, transfer_state transfer) -> transfer_id
  {
    transfer.id       = s.next_transfer++;
    auto const handle = transfer.id;
    if(auto* const pipe = find_pipe(id))
    {
      pipe->queue.push_back(std::move(transfer));
    }
    return handle;
  }

  auto enqueue_control(
      origin                        source,
      std::uint32_t                 owner,
      setup_packet                  setup,
      std::span<std::uint8_t const> data) -> transfer_id
  {
    std::vector<std::byte> stage_data{};
    if(not is_in(setup))
    {
      stage_data = to_byte_vector(data);
      stage_data.resize(setup.length);
    }
    return enqueue(
        default_pipe, {.source = source,
                       .owner  = owner,
                       .setup  = setup,
                       .data   = std::move(stage_data),
                       .length = setup.length});
  }

  [[nodiscard]] auto owns(pipe_state const& pipe, std::size_t driver, std::string_view action) const
      -> bool
  {
    if(pipe.owner == driver)
    {
      return true;
    }
    warn(
        std::format(
            "USB {} on pipe {} from class driver {}, the pipe is driver {}'s: ignored", action,
            pipe.id, driver, pipe.owner));
    return false;
  }

  void attach(std::uint64_t at)
  {
    auto const next_pipe     = s.next_pipe;
    auto const next_transfer = s.next_transfer;
    s                        = {};
    s.next_pipe              = next_pipe;
    s.next_transfer          = next_transfer;
    s.now_ps                 = at;
    s.device                 = device_state::powered;
    s.pipes.push_back(
        {.id = default_pipe, .type = transfer_type::control, .max_packet = first_packet0});
    s.debounce_end = at + to_ps(options.debounce);
    parsed.reset();
    drivers.clear();
  }

  void land(transfer_result result)
  {
    if(auto const found = std::ranges::find(forgotten, result.id); found != forgotten.end())
    {
      forgotten.erase(found);
      return;
    }
    results.push_back(std::move(result));
  }

  void detach(std::uint64_t at)
  {
    for(auto const& pipe : s.pipes)
    {
      for(auto const& transfer : pipe.queue)
      {
        if(transfer.source == origin::user)
        {
          land(
              {.id     = transfer.id,
               .status = transfer_status::cancelled,
               .setup  = transfer.setup,
               .at     = vtime{at},
               .frame  = frame_number()});
        }
        settle_log(transfer.id, transfer_status::cancelled);
      }
    }
    auto const next_pipe     = s.next_pipe;
    auto const next_transfer = s.next_transfer;
    s                        = {};
    s.next_pipe              = next_pipe;
    s.next_transfer          = next_transfer;
    s.now_ps                 = at;
    parsed.reset();
    drivers.clear();
  }

  void settle_log(transfer_id id, transfer_status status)
  {
    auto       newest = entries | std::views::reverse;
    auto const found  = std::ranges::find(newest, id, &log_entry::id);
    if(found != newest.end() and found->kind == log_kind::request and not found->status)
    {
      found->status = status;
    }
  }

  void start_reset(std::uint64_t at)
  {
    s.next_frame.reset();
    s.next_slot.reset();
    s.reset_end = at + to_ps(options.reset_time);
    s.address   = 0;
    if(s.device > device_state::default_)
    {
      s.device = device_state::default_;
    }
    s.configuration = 0;
    drivers.clear();
    s.drivers.clear();
    std::erase_if(s.pipes, [](pipe_state const& pipe) { return pipe.id != default_pipe; });
    auto* const control = find_pipe(default_pipe);
    if(control != nullptr)
    {
      std::erase_if(control->queue, [&](transfer_state const& transfer) {
        if(transfer.source != origin::driver)
        {
          return false;
        }
        settle_log(transfer.id, transfer_status::cancelled);
        return true;
      });
    }
    if(control != nullptr and not control->queue.empty())
    {
      auto& head      = control->queue.front();
      head.step       = stage::setup;
      head.offset     = 0;
      head.errors     = 0;
      head.logged     = false;
      head.started_ps = std::nullopt;
      if(is_in(head.setup))
      {
        head.data.clear();
      }
    }
    entries.push_back({.kind = log_kind::bus_reset, .at = vtime{at}});
    port.bus_reset(vtime{at});
  }

  void finish_reset(std::uint64_t at)
  {
    s.reset_end.reset();
    s.device     = std::max(s.device, device_state::default_);
    s.next_frame = at;
    if(s.step_active)
    {
      s.step_active = false;
      ++s.step;
    }
    advance(at);
  }

  void collect_string_indices()
  {
    s.string_indices.clear();
    auto const add = [&](std::uint8_t index) {
      if(index != 0 and std::ranges::find(s.string_indices, index) == s.string_indices.end())
      {
        s.string_indices.push_back(index);
      }
    };
    if(auto const device = parse_device_descriptor(octets(s.device_bytes)))
    {
      add(device->manufacturer_string);
      add(device->product_string);
      add(device->serial_string);
    }
    if(parsed)
    {
      add(parsed->string_index);
      for(auto const& association : parsed->associations)
      {
        add(association.string_index);
      }
      for(auto const& interface : parsed->interfaces)
      {
        add(interface.string_index);
      }
    }
  }

  [[nodiscard]] auto configuration_value() const -> std::uint8_t
  {
    if(parsed)
    {
      return parsed->value;
    }
    return s.configuration_bytes.size() > 5
             ? std::to_integer<std::uint8_t>(s.configuration_bytes[5])
             : 1;
  }

  void advance(std::uint64_t at)
  {
    using enum enumeration_action;
    while(not s.failed and s.step < options.enumeration.size())
    {
      auto const& step = options.enumeration[s.step];
      s.step_active    = true;
      switch(step.action)
      {
        case get_device_head:
          enqueue_control(
              origin::enumeration, 0, descriptor_request(device_type, 0, 0, first_packet0), {});
          return;
        case bus_reset:
          start_reset(at);
          return;
        case set_address:
          enqueue_control(
              origin::enumeration, 0,
              {.request_type = 0, .request = set_address_request, .value = options.address}, {});
          return;
        case wait:
          s.hold_until = at + to_ps(step.wait);
          return;
        case get_device:
          enqueue_control(
              origin::enumeration, 0, descriptor_request(device_type, 0, 0, device_length), {});
          return;
        case get_configuration_head:
          enqueue_control(
              origin::enumeration, 0,
              descriptor_request(configuration_type, 0, 0, configuration_head), {});
          return;
        case get_configuration:
        {
          auto const total = s.configuration_bytes.size() >= 4
                               ? le16(octets(s.configuration_bytes), 2)
                               : configuration_head;
          enqueue_control(
              origin::enumeration, 0, descriptor_request(configuration_type, 0, 0, total), {});
          return;
        }
        case get_languages:
          enqueue_control(
              origin::enumeration, 0, descriptor_request(string_type, 0, 0, string_length), {});
          return;
        case get_strings:
          if(s.string_cursor == 0)
          {
            collect_string_indices();
          }
          if(s.language != 0 and s.string_cursor < s.string_indices.size())
          {
            enqueue_control(
                origin::enumeration, 0,
                descriptor_request(
                    string_type, s.string_indices[s.string_cursor], s.language, string_length),
                {});
            return;
          }
          s.string_cursor = 0;
          break;
        case set_configuration:
          enqueue_control(
              origin::enumeration, 0,
              {.request_type = 0,
               .request      = set_configuration_request,
               .value        = configuration_value()},
              {});
          return;
      }
      s.step_active = false;
      ++s.step;
    }
    s.step_active = false;
    s.enumerated  = not s.failed;
  }

  void learn_packet0(std::span<std::uint8_t const> bytes)
  {
    if(bytes.size() <= max_packet0_offset)
    {
      return;
    }
    auto const size = bytes[max_packet0_offset];
    if(size != 8 and size != 16 and size != 32 and size != 64)
    {
      warn(
          std::format(
              "bMaxPacketSize0 {} is not 8, 16, 32 or 64: EP0 keeps {}", size, first_packet0));
      return;
    }
    if(auto* const control = find_pipe(default_pipe))
    {
      control->max_packet = size;
    }
  }

  void enumeration_done(transfer_result const& result)
  {
    using enum enumeration_action;
    auto const at     = result.at.ps;
    auto const action = options.enumeration[s.step].action;
    if(result.status != transfer_status::completed)
    {
      if(result.status == transfer_status::stalled and action == get_strings)
      {
        ++s.string_cursor;
        advance(at);
        return;
      }
      if(result.status == transfer_status::stalled and action == get_languages)
      {
        s.language    = 0;
        s.step_active = false;
        ++s.step;
        advance(at);
        return;
      }
      warn(
          std::format(
              "USB enumeration stopped at {}: request {:#04x} {:#04x} {:#06x} {}",
              reflex::enum_value_name(action), result.setup.request_type, result.setup.request,
              result.setup.value, reflex::enum_value_name(result.status)));
      s.failed      = action;
      s.step_active = false;
      return;
    }
    switch(action)
    {
      case get_device_head:
      case get_device:
        s.device_bytes = to_byte_vector(result.data);
        learn_packet0(result.data);
        break;
      case set_address:
        s.address = options.address;
        s.device  = device_state::address;
        break;
      case get_configuration_head:
        s.configuration_bytes = to_byte_vector(result.data);
        break;
      case get_configuration:
      {
        s.configuration_bytes = to_byte_vector(result.data);
        auto config           = parse_configuration(octets(s.configuration_bytes));
        if(not config)
        {
          warn(std::format("USB enumeration stopped: configuration descriptor {}", config.error()));
          s.failed      = action;
          s.step_active = false;
          return;
        }
        parsed = std::move(*config);
        break;
      }
      case get_languages:
        s.strings.push_back({.index = 0, .bytes = to_byte_vector(result.data)});
        s.language = result.data.size() >= 4 ? le16(result.data, 2) : 0;
        break;
      case get_strings:
        s.strings.push_back(
            {.index = s.string_indices[s.string_cursor], .bytes = to_byte_vector(result.data)});
        ++s.string_cursor;
        advance(at);
        return;
      case set_configuration:
        s.configuration = static_cast<std::uint8_t>(result.setup.value);
        s.device        = device_state::configured;
        s.step_active   = false;
        ++s.step;
        bind_drivers();
        advance(at);
        return;
      case bus_reset:
      case wait:
        break;
    }
    s.step_active = false;
    ++s.step;
    advance(at);
  }

  [[nodiscard]] auto claimable(
      std::string const&            name,
      std::span<std::uint8_t const> claimed,
      std::span<std::uint8_t const> taken) const -> bool
  {
    for(std::size_t at = 0; at < claimed.size(); ++at)
    {
      auto const number  = claimed[at];
      auto const missing = parsed->find(number) == nullptr;
      auto const busy =
          std::ranges::contains(taken, number) or std::ranges::contains(claimed.first(at), number);
      if(missing or busy)
      {
        warn(
            std::format(
                "USB class driver {} claims interface {}, which is {}: not bound", name, number,
                missing ? "not in the configuration" : "already taken"));
        return false;
      }
    }
    return true;
  }

  void load_factories()
  {
    for(auto const& factory : options.drivers)
    {
      if(not factory.load)
      {
        continue;
      }
      auto const found = std::ranges::find(s.factories, factory.name, &factory_record::name);
      factory.load(found == s.factories.end() ? std::span<std::byte const>{} : found->bytes);
    }
    s.factories.clear();
  }

  void bind_drivers()
  {
    drivers.clear();
    s.drivers.clear();
    if(not parsed)
    {
      return;
    }
    std::vector<std::uint8_t> taken{};
    for(auto const& interface : parsed->interfaces)
    {
      if(interface.alternate != 0 or std::ranges::contains(taken, interface.number))
      {
        continue;
      }
      for(auto const& factory : options.drivers)
      {
        if(not factory.claim or not factory.make)
        {
          continue;
        }
        auto claimed = factory.claim(*parsed, interface);
        if(claimed.empty() or not claimable(factory.name, claimed, taken))
        {
          continue;
        }
        taken.insert(taken.end(), claimed.begin(), claimed.end());
        s.drivers.push_back({.name = factory.name, .interfaces = claimed});
        drivers.push_back(
            {.name = factory.name, .interfaces = std::move(claimed), .driver = factory.make()});
        break;
      }
    }
    for(std::size_t index = 0; index < drivers.size(); ++index)
    {
      if(drivers[index].driver)
      {
        host_context ctx{self, index};
        drivers[index].driver->configured(ctx);
      }
    }
  }

  void deliver(pipe_id id, std::uint32_t owner, origin source, transfer_result const& result)
  {
    settle_log(result.id, result.status);
    if(source == origin::enumeration)
    {
      enumeration_done(result);
      return;
    }
    if(source == origin::user)
    {
      land(result);
      return;
    }
    if(owner >= drivers.size() or not drivers[owner].driver)
    {
      return;
    }
    host_context ctx{self, owner};
    if(id == default_pipe)
    {
      drivers[owner].driver->control_done(ctx, result);
    }
    else
    {
      drivers[owner].driver->transfer_done(ctx, id, result);
    }
  }

  void finish(pipe_id id, transfer_status status, std::uint64_t at)
  {
    auto* const pipe = find_pipe(id);
    if(pipe == nullptr or pipe->queue.empty())
    {
      return;
    }
    auto transfer = std::move(pipe->queue.front());
    pipe->queue.erase(pipe->queue.begin());
    auto const owner = pipe->owner;
    deliver(
        id, id == default_pipe ? transfer.owner : owner, transfer.source,
        {.id     = transfer.id,
         .status = status,
         .setup  = transfer.setup,
         .data   = copy_octets(transfer.data),
         .at     = vtime{at},
         .frame  = frame_number()});
  }

  auto no_answer(transfer_state& transfer, std::uint64_t time) -> outcome
  {
    ++transfer.errors;
    if(transfer.errors >= error_limit)
    {
      return {.time = time, .progress = true, .done = transfer_status::error};
    }
    return {.time = time};
  }

  auto control_transaction(pipe_state& pipe, std::uint64_t at) -> outcome
  {
    auto& transfer = pipe.queue.front();
    if(not transfer.started_ps)
    {
      transfer.started_ps = at;
    }
    auto const in = is_in(transfer.setup);
    switch(transfer.step)
    {
      case stage::setup:
      {
        if(not transfer.logged)
        {
          transfer.logged = true;
          entries.push_back(
              {.kind    = log_kind::request,
               .at      = vtime{at},
               .address = s.address,
               .setup   = transfer.setup,
               .id      = transfer.id});
        }
        auto const time = bus_time(transfer_type::control, false, 8);
        switch(port.setup(s.address, transfer.setup, vtime{at}))
        {
          case handshake::ack:
            transfer.errors = 0;
            transfer.offset = 0;
            pipe.toggle     = data_pid::data1;
            transfer.step   = transfer.setup.length == 0 ? stage::status : stage::data;
            return {.time = time, .progress = true};
          case handshake::stall:
            return {.time = time, .progress = true, .done = transfer_status::stalled};
          case handshake::nak:
            return {.time = time};
          case handshake::none:
            return no_answer(transfer, time);
        }
        break;
      }
      case stage::data:
        if(in)
        {
          auto       answer = port.in(s.address, 0, vtime{at});
          auto const time   = bus_time(transfer_type::control, true, answer.data.size());
          if(answer.hs != handshake::ack)
          {
            return answer.hs == handshake::nak ? outcome{.time = time}
                 : answer.hs == handshake::stall
                     ? outcome{.time = time, .progress = true, .done = transfer_status::stalled}
                     : no_answer(transfer, time);
          }
          transfer.errors = 0;
          if(answer.pid != pipe.toggle)
          {
            return {.time = time, .progress = true};
          }
          pipe.toggle          = flip(pipe.toggle);
          auto const remaining = transfer.length - transfer.data.size();
          auto const packet    = std::as_bytes(std::span{answer.data});
          if(packet.size() > remaining)
          {
            transfer.data.append_range(packet.first(remaining));
            return {.time = time, .progress = true, .done = transfer_status::overflow};
          }
          transfer.data.append_range(packet);
          if(answer.data.size() < pipe.max_packet or transfer.data.size() == transfer.length)
          {
            transfer.step = stage::status;
            pipe.toggle   = data_pid::data1;
          }
          return {.time = time, .progress = true};
        }
        else
        {
          auto const chunk =
              std::min<std::size_t>(pipe.max_packet, transfer.data.size() - transfer.offset);
          auto const time = bus_time(transfer_type::control, false, chunk);
          auto const hs   = port.out(
              s.address, 0, octets(transfer.data).subspan(transfer.offset, chunk), pipe.toggle,
              vtime{at});
          if(hs != handshake::ack)
          {
            return hs == handshake::nak ? outcome{.time = time}
                 : hs == handshake::stall
                     ? outcome{.time = time, .progress = true, .done = transfer_status::stalled}
                     : no_answer(transfer, time);
          }
          transfer.errors = 0;
          transfer.offset += static_cast<std::uint32_t>(chunk);
          pipe.toggle = flip(pipe.toggle);
          if(transfer.offset >= transfer.data.size())
          {
            transfer.step = stage::status;
            pipe.toggle   = data_pid::data1;
          }
          return {.time = time, .progress = true};
        }
      case stage::status:
      {
        handshake  hs{};
        auto const time = bus_time(transfer_type::control, not(in and transfer.length != 0), 0);
        if(in and transfer.length != 0)
        {
          hs = port.out(s.address, 0, {}, data_pid::data1, vtime{at});
        }
        else
        {
          hs = port.in(s.address, 0, vtime{at}).hs;
        }
        switch(hs)
        {
          case handshake::ack:
            return {.time = time, .progress = true, .done = transfer_status::completed};
          case handshake::stall:
            return {.time = time, .progress = true, .done = transfer_status::stalled};
          case handshake::nak:
            return {.time = time};
          case handshake::none:
            return no_answer(transfer, time);
        }
        break;
      }
    }
    return {};
  }

  auto data_transaction(pipe_state& pipe, std::uint64_t at) -> outcome
  {
    auto&      transfer = pipe.queue.front();
    auto const endpoint = static_cast<std::uint8_t>(pipe.endpoint & 0x0fu);
    auto const iso      = pipe.type == transfer_type::isochronous;
    if((pipe.endpoint & device_to_host) != 0)
    {
      auto       answer = port.in(s.address, endpoint, vtime{at});
      auto const time   = bus_time(pipe.type, true, answer.data.size());
      if(iso)
      {
        if(answer.hs != handshake::ack)
        {
          return {.time = time, .progress = true, .done = transfer_status::error};
        }
        transfer.data = to_byte_vector(answer.data);
        return {.time = time, .progress = true, .done = transfer_status::completed};
      }
      if(answer.hs != handshake::ack)
      {
        return answer.hs == handshake::nak ? outcome{.time = time}
             : answer.hs == handshake::stall
                 ? outcome{.time = time, .progress = true, .done = transfer_status::stalled}
                 : no_answer(transfer, time);
      }
      transfer.errors = 0;
      if(answer.pid != pipe.toggle)
      {
        return {.time = time, .progress = true};
      }
      pipe.toggle          = flip(pipe.toggle);
      auto const remaining = transfer.length - transfer.data.size();
      auto const packet    = std::as_bytes(std::span{answer.data});
      if(packet.size() > remaining)
      {
        transfer.data.append_range(packet.first(remaining));
        return {.time = time, .progress = true, .done = transfer_status::overflow};
      }
      transfer.data.append_range(packet);
      if(answer.data.size() < pipe.max_packet or transfer.data.size() == transfer.length)
      {
        return {.time = time, .progress = true, .done = transfer_status::completed};
      }
      return {.time = time, .progress = true};
    }
    if(iso)
    {
      auto const time = bus_time(pipe.type, false, transfer.data.size());
      auto const hs =
          port.out(s.address, endpoint, octets(transfer.data), data_pid::data0, vtime{at});
      return {
          .time     = time,
          .progress = true,
          .done     = hs == handshake::ack ? transfer_status::completed : transfer_status::error};
    }
    auto const chunk =
        std::min<std::size_t>(pipe.max_packet, transfer.data.size() - transfer.offset);
    auto const time = bus_time(pipe.type, false, chunk);
    auto const hs   = port.out(
        s.address, endpoint, octets(transfer.data).subspan(transfer.offset, chunk), pipe.toggle,
        vtime{at});
    if(hs != handshake::ack)
    {
      return hs == handshake::nak ? outcome{.time = time}
           : hs == handshake::stall
               ? outcome{.time = time, .progress = true, .done = transfer_status::stalled}
               : no_answer(transfer, time);
    }
    transfer.errors = 0;
    pipe.toggle     = flip(pipe.toggle);
    transfer.offset += static_cast<std::uint32_t>(chunk);
    if(chunk == 0)
    {
      transfer.zlp_sent = true;
    }
    auto const size      = transfer.data.size();
    auto const needs_zlp = size == 0 or (transfer.zero_packet and size % pipe.max_packet == 0);
    if(transfer.offset >= size and (not needs_zlp or transfer.zlp_sent))
    {
      return {.time = time, .progress = true, .done = transfer_status::completed};
    }
    return {.time = time, .progress = true};
  }

  auto transact(pipe_state& pipe, std::uint64_t at) -> outcome
  {
    return pipe.type == transfer_type::control ? control_transaction(pipe, at)
                                               : data_transaction(pipe, at);
  }

  [[nodiscard]] static auto worst_time(pipe_state const& pipe) -> std::uint64_t
  {
    return bus_time(pipe.type, false, pipe.max_packet);
  }

  [[nodiscard]] auto pick() const -> std::optional<std::size_t>
  {
    auto const  count = s.pipes.size();
    std::size_t start = 0;
    if(s.last_served)
    {
      auto const found = std::ranges::find(s.pipes, *s.last_served, &pipe_state::id);
      if(found != s.pipes.end())
      {
        start = static_cast<std::size_t>(found - s.pipes.begin()) + 1;
      }
    }
    for(std::size_t offset = 0; offset < count; ++offset)
    {
      auto const index = (start + offset) % count;
      if(non_periodic(s.pipes[index]) and not s.pipes[index].queue.empty())
      {
        return index;
      }
    }
    return std::nullopt;
  }

  void plan_slot(std::uint64_t at)
  {
    s.next_slot.reset();
    if(not s.next_frame)
    {
      return;
    }
    auto const busy = busy_pipes();
    if(busy == 0)
    {
      return;
    }
    if(s.nak_streak >= busy)
    {
      s.nak_streak = 0;
      at += async_sleep_ps;
    }
    auto const index = pick();
    if(index and at + worst_time(s.pipes[*index]) <= *s.next_frame)
    {
      s.next_slot = at;
    }
  }

  void run_slot(std::uint64_t at)
  {
    s.next_slot.reset();
    auto const index = pick();
    if(not index or not s.next_frame or at + worst_time(s.pipes[*index]) > *s.next_frame)
    {
      return;
    }
    auto&      pipe   = s.pipes[*index];
    auto const id     = pipe.id;
    auto const result = transact(pipe, at);
    s.last_served     = id;
    s.nak_streak      = result.progress ? 0 : s.nak_streak + 1;
    if(result.done)
    {
      finish(id, *result.done, at);
    }
    plan_slot(at + result.time);
  }

  void check_timeout(std::uint64_t at)
  {
    auto* const control = find_pipe(default_pipe);
    if(control == nullptr or control->queue.empty())
    {
      return;
    }
    auto const& head = control->queue.front();
    if(head.started_ps and at - *head.started_ps >= to_ps(options.request_timeout))
    {
      finish(default_pipe, transfer_status::timed_out, at);
    }
  }

  void run_frame(std::uint64_t at)
  {
    ++s.frame_index;
    s.next_frame = at + frame_ps;
    s.nak_streak = 0;
    port.start_of_frame(frame_number(), vtime{at});
    check_timeout(at);
    if(s.device == device_state::configured)
    {
      for(std::size_t index = 0; index < drivers.size(); ++index)
      {
        if(drivers[index].driver)
        {
          host_context ctx{self, index};
          drivers[index].driver->frame(ctx, frame_number());
        }
      }
    }
    auto       cursor = at + sof_ps;
    auto const limit  = at + frame_ps * options.periodic_percent / 100;
    for(auto const type : {transfer_type::isochronous, transfer_type::interrupt})
    {
      std::vector<pipe_id> ids{};
      for(auto const& pipe : s.pipes)
      {
        if(pipe.type == type)
        {
          ids.push_back(pipe.id);
        }
      }
      for(auto const id : ids)
      {
        auto* const pipe = find_pipe(id);
        if(pipe == nullptr or pipe->queue.empty() or s.frame_index < pipe->next_poll)
        {
          continue;
        }
        auto const& head = pipe->queue.front();
        auto const  size =
            (pipe->endpoint & device_to_host) != 0
                ? pipe->max_packet
                : std::min<std::size_t>(pipe->max_packet, head.data.size() - head.offset);
        auto const cost = bus_time(type, (pipe->endpoint & device_to_host) != 0, size);
        if(cursor + cost > limit)
        {
          if(type == transfer_type::isochronous)
          {
            pipe->next_poll = s.frame_index + pipe->period;
            if(not std::exchange(pipe->overran, true))
            {
              warn(
                  std::format(
                      "USB isochronous pipe {} to endpoint {:#04x}: a {}-byte packet does not "
                      "fit before the frame's EOPF point, it ends error, as every packet that "
                      "does not fit on this pipe",
                      id, pipe->endpoint, size));
            }
            finish(id, transfer_status::error, cursor);
          }
          continue;
        }
        pipe->next_poll   = s.frame_index + pipe->period;
        s.now_ps          = cursor;
        auto const result = transact(*pipe, cursor);
        if(result.done)
        {
          finish(id, *result.done, cursor);
        }
        cursor += result.time;
      }
    }
    plan_slot(cursor);
  }

  void on_time(std::uint64_t at)
  {
    s.now_ps = at;
    if(s.debounce_end and *s.debounce_end <= at)
    {
      s.debounce_end.reset();
      start_reset(at);
    }
    if(s.reset_end and *s.reset_end <= at)
    {
      finish_reset(*s.reset_end);
    }
    if(s.hold_until and *s.hold_until <= at)
    {
      s.hold_until.reset();
      s.step_active = false;
      ++s.step;
      advance(at);
      if(not s.next_slot and s.next_frame and *s.next_frame > at)
      {
        plan_slot(at);
      }
    }
    if(s.next_frame and *s.next_frame <= at)
    {
      run_frame(*s.next_frame);
    }
    else if(s.next_slot and *s.next_slot <= at)
    {
      run_slot(at);
    }
    schedule();
  }
};

namespace
{
thread_local deferred_warnings* innermost_deferral = nullptr;
}

deferred_warnings::deferred_warnings() noexcept
    : outer_{std::exchange(innermost_deferral, this)}, exceptions_{std::uncaught_exceptions()}
{}

deferred_warnings::~deferred_warnings() noexcept(false)
{
  innermost_deferral = outer_;
  if(outer_ != nullptr)
  {
    outer_->held_.insert(
        outer_->held_.end(), std::make_move_iterator(held_.begin()),
        std::make_move_iterator(held_.end()));
    return;
  }
  auto const unwinding = std::uncaught_exceptions() > exceptions_;
  for(auto const& warning : held_)
  {
    if(not unwinding)
    {
      (*warning.sink)(warning.message);
      continue;
    }
    try
    {
      (*warning.sink)(warning.message);
    }
    catch(...)
    {
      return;
    }
  }
}

auto deferred_warnings::hold(
    std::function<void(std::string const&)> const& sink,
    std::string const&                             message) -> bool
{
  if(innermost_deferral == nullptr)
  {
    return false;
  }
  innermost_deferral->held_.push_back({.sink = &sink, .message = message});
  return true;
}

host_context::host_context(host& owner, std::size_t driver) noexcept
    : host_{&owner}, driver_{driver}
{}

auto host_context::interfaces() const -> std::span<std::uint8_t const>
{
  auto const& drivers = host_->impl_->drivers;
  if(driver_ >= drivers.size())
  {
    return {};
  }
  return drivers[driver_].interfaces;
}

auto host_context::configuration() const -> configuration_info const&
{
  static configuration_info const none{};
  auto const&                     parsed = host_->impl_->parsed;
  return parsed ? *parsed : none;
}

auto host_context::open_pipe(endpoint_info const& endpoint) -> pipe_id
{
  auto&      s  = host_->impl_->s;
  auto       id = s.next_pipe++;
  auto const first_poll =
      endpoint.type == transfer_type::isochronous ? iso_start_frames : std::uint64_t{1};
  s.pipes.push_back(
      {.id         = id,
       .owner      = static_cast<std::uint32_t>(driver_),
       .endpoint   = endpoint.address,
       .type       = endpoint.type,
       .max_packet = endpoint.max_packet,
       .period     = period_of(endpoint),
       .next_poll  = s.frame_index + first_poll});
  return id;
}

void host_context::close_pipe(pipe_id pipe)
{
  auto&             impl  = *host_->impl_;
  auto const* const found = impl.find_pipe(pipe);
  if(pipe == default_pipe or found == nullptr or not impl.owns(*found, driver_, "close"))
  {
    return;
  }
  std::erase_if(impl.s.pipes, [&](pipe_state const& entry) { return entry.id == pipe; });
}

auto host_context::submit(pipe_id pipe, transfer_request request) -> transfer_id
{
  auto&             impl  = *host_->impl_;
  auto const* const found = pipe == default_pipe ? nullptr : impl.find_pipe(pipe);
  if(found == nullptr)
  {
    impl.warn(std::format("USB transfer on pipe {}, which is not open: dropped", pipe));
    return 0;
  }
  if(not impl.owns(*found, driver_, "transfer"))
  {
    return 0;
  }
  return impl.enqueue(
      pipe, {.source      = origin::driver,
             .owner       = static_cast<std::uint32_t>(driver_),
             .data        = to_byte_vector(request.data),
             .length      = request.length,
             .zero_packet = request.zero_packet});
}

auto host_context::control(setup_packet setup, std::vector<std::uint8_t> data) -> transfer_id
{
  return host_->impl_->enqueue_control(
      origin::driver, static_cast<std::uint32_t>(driver_), setup, data);
}

void host_context::reset_toggle(pipe_id pipe)
{
  auto&       impl  = *host_->impl_;
  auto* const found = pipe == default_pipe ? nullptr : impl.find_pipe(pipe);
  if(found == nullptr)
  {
    impl.warn(
        std::format("USB toggle reset on pipe {}, which is not an open data pipe: ignored", pipe));
    return;
  }
  if(impl.owns(*found, driver_, "toggle reset"))
  {
    found->toggle = data_pid::data0;
  }
}

auto host_context::queued(pipe_id pipe) const -> std::size_t
{
  auto const* const found = std::as_const(*host_->impl_).find_pipe(pipe);
  return found == nullptr ? 0 : found->queue.size();
}

auto host_context::now() const -> vtime
{
  return vtime{host_->impl_->s.now_ps};
}

auto host_context::frame() const -> std::uint16_t
{
  return host_->impl_->frame_number();
}

void host_context::warn(std::string const& message) const
{
  host_->impl_->warn(message);
}

host::host(device_port& port, host_options options)
    : impl_{std::in_place, *this, port, std::move(options)}
{}

host::~host() = default;

void host::pull_up_changed(bool on, vtime at)
{
  if(on == impl_->attached())
  {
    return;
  }
  if(on)
  {
    impl_->attach(at.ps);
  }
  else
  {
    impl_->detach(at.ps);
  }
  impl_->schedule();
}

void host::on_time(vtime at)
{
  impl_->on_time(at.ps);
}

void host::save(std::vector<std::byte>& out) const
{
  auto saved = impl_->s;
  for(auto const& factory : impl_->options.drivers)
  {
    if(factory.save)
    {
      factory.save(saved.factories.emplace_back(factory_record{.name = factory.name}).bytes);
    }
  }
  for(std::size_t index = 0; index < saved.drivers.size() and index < impl_->drivers.size();
      ++index)
  {
    if(auto const& driver = impl_->drivers[index].driver)
    {
      driver->save(saved.drivers[index].bytes);
    }
  }
  reflex::serde::bson::serializer{out}.dump(saved);
}

void host::load(std::span<std::byte const> in)
{
  auto& impl = *impl_;
  if(in.empty())
  {
    impl.detach(impl.port.now().ps);
    impl.results.clear();
    impl.forgotten.clear();
    ++impl.epoch;
    impl.load_factories();
    return;
  }
  impl.s = reflex::serde::bson::deserializer{in}.template load<host_state>();
  impl.load_factories();
  impl.results.clear();
  impl.forgotten.clear();
  ++impl.epoch;
  impl.parsed.reset();
  if(auto config = parse_configuration(octets(impl.s.configuration_bytes)))
  {
    impl.parsed = std::move(*config);
  }
  auto const kept_at = [&](std::size_t index) {
    if(index >= impl.drivers.size() or index >= impl.s.drivers.size())
    {
      return false;
    }
    auto const& current = impl.drivers[index];
    auto const& record  = impl.s.drivers[index];
    return current.name == record.name and current.interfaces == record.interfaces;
  };
  for(std::size_t index = 0; index < impl.drivers.size(); ++index)
  {
    if(not kept_at(index))
    {
      impl.drivers[index].driver.reset();
    }
  }
  std::vector<bound_driver> drivers{};
  for(std::size_t index = 0; index < impl.s.drivers.size(); ++index)
  {
    auto& record = impl.s.drivers[index];
    if(kept_at(index))
    {
      drivers.push_back(std::move(impl.drivers[index]));
    }
    else
    {
      auto const factory =
          std::ranges::find(impl.options.drivers, record.name, &driver_factory::name);
      auto const buildable = factory != impl.options.drivers.end() and factory->make;
      if(not buildable)
      {
        impl.warn(std::format("USB host restore: no class driver named {}", record.name));
      }
      drivers.push_back(
          {.name       = record.name,
           .interfaces = record.interfaces,
           .driver     = buildable ? factory->make() : nullptr});
    }
    if(drivers.back().driver)
    {
      drivers.back().driver->load(record.bytes);
    }
    record.bytes.clear();
  }
  impl.drivers = std::move(drivers);
}

auto host::state() const -> device_state
{
  return impl_->s.device;
}

auto host::address() const -> std::uint8_t
{
  return impl_->s.address;
}

auto host::configuration_value() const -> std::uint8_t
{
  return impl_->s.configuration;
}

auto host::enumerated() const -> bool
{
  return impl_->s.enumerated;
}

auto host::failed_step() const -> std::optional<enumeration_action>
{
  return impl_->s.failed;
}

auto host::device_bytes() const -> std::span<std::uint8_t const>
{
  return octets(impl_->s.device_bytes);
}

auto host::configuration_bytes() const -> std::span<std::uint8_t const>
{
  return octets(impl_->s.configuration_bytes);
}

auto host::configuration() const -> configuration_info const*
{
  return impl_->parsed ? &*impl_->parsed : nullptr;
}

auto host::strings() const -> std::span<string_descriptor const>
{
  return impl_->s.strings;
}

auto host::language() const -> std::uint16_t
{
  return impl_->s.language;
}

auto host::control(setup_packet setup, std::vector<std::uint8_t> data) -> transfer_id
{
  auto& impl = *impl_;
  if(not impl.attached())
  {
    auto const id = impl.s.next_transfer++;
    impl.results.push_back(
        {.id     = id,
         .status = transfer_status::cancelled,
         .setup  = setup,
         .at     = vtime{impl.s.now_ps},
         .frame  = impl.frame_number()});
    return id;
  }
  return impl.enqueue_control(origin::user, 0, setup, data);
}

auto host::take_results() -> std::vector<transfer_result>
{
  return std::exchange(impl_->results, {});
}

auto host::has_result(transfer_id id) const -> bool
{
  return std::ranges::any_of(
      impl_->results, [id](transfer_result const& result) { return result.id == id; });
}

auto host::take_result(transfer_id id) -> std::optional<transfer_result>
{
  auto const found = std::ranges::find(impl_->results, id, &transfer_result::id);
  if(found == impl_->results.end())
  {
    return std::nullopt;
  }
  auto taken = std::move(*found);
  impl_->results.erase(found);
  return taken;
}

void host::forget(transfer_id id)
{
  if(not take_result(id) and not std::ranges::contains(impl_->forgotten, id))
  {
    impl_->forgotten.push_back(id);
  }
}

auto host::forgotten() const noexcept -> std::size_t
{
  return impl_->forgotten.size();
}

auto host::epoch() const noexcept -> std::uint64_t
{
  return impl_->epoch;
}

auto host::log() const -> std::span<log_entry const>
{
  return impl_->entries;
}

void host::clear_log()
{
  impl_->entries.clear();
}

auto host::drivers() const -> std::span<bound_driver const>
{
  return impl_->drivers;
}

auto host::context(std::size_t driver) -> host_context
{
  return host_context{*this, driver};
}
}
