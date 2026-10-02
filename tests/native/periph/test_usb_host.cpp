#include <popcorn/periph/usb.hpp>
#include <popcorn/periph/usb_host.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
namespace usb = popcorn::periph::usb;
using popcorn::vtime;

constexpr std::uint64_t us     = 1'000'000;
constexpr std::uint64_t ms     = 1000 * us;
constexpr std::uint64_t second = 1000 * ms;
constexpr std::uint64_t t0     = 3 * ms;

enum class token : std::uint8_t
{
  setup,
  in,
  out,
};

struct token_record
{
  std::uint64_t  at{};
  token          kind{};
  std::uint8_t   endpoint{};
  usb::handshake hs{};
};

struct request_record
{
  std::uint64_t     at{};
  std::uint8_t      address{};
  usb::setup_packet setup{};

  friend auto operator==(request_record const&, request_record const&) -> bool = default;
};

struct device_script
{
  std::uint8_t                                                   address{};
  std::optional<std::uint8_t>                                    next_address{};
  std::optional<std::uint64_t>                                   wake{};
  std::optional<usb::setup_packet>                               request{};
  std::vector<std::uint8_t>                                      reply{};
  std::size_t                                                    sent{};
  bool                                                           data_done{};
  bool                                                           stalled{};
  usb::data_pid                                                  toggle{};
  std::uint32_t                                                  data_naks{};
  bool                                                           silent{};
  bool                                                           stall_class{};
  std::vector<std::uint8_t>                                      stalled_strings{};
  std::uint8_t                                                   configuration{};
  std::vector<request_record>                                    requests{};
  std::vector<std::uint64_t>                                     resets{};
  std::vector<std::uint64_t>                                     frames{};
  std::vector<token_record>                                      tokens{};
  std::map<std::uint8_t, std::deque<std::vector<std::uint8_t>>>  in_queue{};
  std::map<std::uint8_t, usb::data_pid>                          in_toggle{};
  std::map<std::uint8_t, std::vector<std::vector<std::uint8_t>>> received{};
  std::vector<std::uint8_t>                                      iso_endpoints{};
  std::vector<std::uint8_t>                                      class_out{};
};

auto utf16(std::string_view text) -> std::vector<std::uint8_t>
{
  std::vector<std::uint8_t> bytes{static_cast<std::uint8_t>(2 + 2 * text.size()), 3};
  for(auto const c : text)
  {
    bytes.push_back(static_cast<std::uint8_t>(c));
    bytes.push_back(0);
  }
  return bytes;
}

auto device_bytes() -> std::vector<std::uint8_t>
{
  return {18, 1, 0x00, 0x02, 0xef, 0x02, 0x01, 64, 0x09, 0x12, 0x01, 0x00, 0x00, 0x01, 1, 2, 3, 1};
}

auto configuration_bytes() -> std::vector<std::uint8_t>
{
  std::vector<std::uint8_t> bytes{
      9,    2,  0,    0,    3,    1,    0,  0x80, 50,   8,    11,   0,    2,    2,    2,
      1,    0,  9,    4,    0,    0,    1,  2,    2,    1,    4,    5,    0x24, 0x00, 0x10,
      0x01, 5,  0x24, 0x01, 0x00, 0x01, 4,  0x24, 0x02, 0x02, 5,    0x24, 0x06, 0x00, 0x01,
      7,    5,  0x81, 3,    8,    0,    16, 9,    4,    1,    0,    2,    0x0a, 0,    0,
      0,    7,  5,    0x02, 2,    64,   0,  0,    7,    5,    0x82, 2,    64,   0,    0,
      9,    4,  2,    0,    1,    3,    0,  0,    5,    9,    0x21, 0x11, 0x01, 0,    1,
      0x22, 25, 0,    7,    5,    0x83, 3,  8,    0,    10};
  bytes[2] = static_cast<std::uint8_t>(bytes.size());
  bytes[3] = static_cast<std::uint8_t>(bytes.size() >> 8);
  return bytes;
}

auto string_bytes(std::uint8_t index) -> std::optional<std::vector<std::uint8_t>>
{
  switch(index)
  {
    case 0:
      return std::vector<std::uint8_t>{4, 3, 0x09, 0x04};
    case 1:
      return utf16("popcorn");
    case 2:
      return utf16("composite");
    case 3:
      return utf16("0001");
    case 4:
      return utf16("acm");
    case 5:
      return utf16("hid");
    default:
      return std::nullopt;
  }
}

auto flip(usb::data_pid pid) -> usb::data_pid
{
  return pid == usb::data_pid::data0 ? usb::data_pid::data1 : usb::data_pid::data0;
}

class fake_device final : public usb::device_port
{
public:
  device_script s;

  void attach(std::shared_ptr<usb::host_side>) override
  {}

  [[nodiscard]] auto pulled_up() const -> bool override
  {
    return true;
  }

  void bus_reset(vtime at) override
  {
    s.resets.push_back(at.ps);
    s.address = 0;
    s.next_address.reset();
    s.request.reset();
  }

  void start_of_frame(std::uint16_t, vtime at) override
  {
    s.frames.push_back(at.ps);
  }

  auto setup(std::uint8_t address, usb::setup_packet packet, vtime at) -> usb::handshake override
  {
    if(address != s.address)
    {
      s.tokens.push_back({at.ps, token::setup, 0, usb::handshake::none});
      return usb::handshake::none;
    }
    s.tokens.push_back({at.ps, token::setup, 0, usb::handshake::ack});
    s.requests.push_back({at.ps, address, packet});
    s.request   = packet;
    s.sent      = 0;
    s.data_done = false;
    s.stalled   = false;
    s.toggle    = usb::data_pid::data1;
    s.reply.clear();
    auto const kind = (packet.request_type >> 5) & 3u;
    if(kind == 1)
    {
      s.stalled = s.stall_class;
      return usb::handshake::ack;
    }
    if(packet.request_type == 0x80 and packet.request == 6)
    {
      auto const type  = packet.value >> 8;
      auto const index = static_cast<std::uint8_t>(packet.value);
      if(type == 1)
      {
        s.reply = device_bytes();
      }
      else if(type == 2)
      {
        s.reply = configuration_bytes();
      }
      else if(type == 3 and not std::ranges::contains(s.stalled_strings, index))
      {
        if(auto bytes = string_bytes(index))
        {
          s.reply = std::move(*bytes);
        }
        else
        {
          s.stalled = true;
        }
      }
      else
      {
        s.stalled = true;
      }
      if(s.reply.size() > packet.length)
      {
        s.reply.resize(packet.length);
      }
    }
    else if(packet.request_type == 0 and packet.request == 5)
    {
      s.next_address = static_cast<std::uint8_t>(packet.value);
    }
    else if(packet.request_type == 0 and packet.request == 9)
    {
      s.configuration = static_cast<std::uint8_t>(packet.value);
    }
    else if(packet.request_type != 1 or packet.request != 11)
    {
      s.stalled = true;
    }
    return usb::handshake::ack;
  }

  auto
      out(std::uint8_t                  address,
          std::uint8_t                  endpoint,
          std::span<std::uint8_t const> data,
          usb::data_pid,
          vtime at) -> usb::handshake override
  {
    auto const hs = answer_out(address, endpoint, data);
    s.tokens.push_back({at.ps, token::out, endpoint, hs});
    return hs;
  }

  auto in(std::uint8_t address, std::uint8_t endpoint, vtime at) -> usb::in_result override
  {
    auto result = answer_in(address, endpoint);
    s.tokens.push_back({at.ps, token::in, endpoint, result.hs});
    return result;
  }

  void suspend(vtime) override
  {}

  void resume(vtime) override
  {}

  void wake_at(vtime at) override
  {
    s.wake = at.ps;
  }

private:
  auto answer_out(std::uint8_t address, std::uint8_t endpoint, std::span<std::uint8_t const> data)
      -> usb::handshake
  {
    if(address != s.address)
    {
      return usb::handshake::none;
    }
    if(endpoint == 0)
    {
      if(s.stalled)
      {
        return usb::handshake::stall;
      }
      s.class_out.insert(s.class_out.end(), data.begin(), data.end());
      return usb::handshake::ack;
    }
    s.received[endpoint].emplace_back(data.begin(), data.end());
    return usb::handshake::ack;
  }

  auto answer_in(std::uint8_t address, std::uint8_t endpoint) -> usb::in_result
  {
    if(address != s.address)
    {
      return {};
    }
    if(endpoint != 0)
    {
      auto& queue = s.in_queue[endpoint];
      if(std::ranges::contains(s.iso_endpoints, endpoint))
      {
        if(queue.empty())
        {
          return {};
        }
        usb::in_result result{.hs = usb::handshake::ack, .data = std::move(queue.front())};
        queue.pop_front();
        return result;
      }
      if(queue.empty())
      {
        return {.hs = usb::handshake::nak, .data = {}};
      }
      auto&          toggle = s.in_toggle[endpoint];
      usb::in_result result{
          .hs = usb::handshake::ack, .pid = toggle, .data = std::move(queue.front())};
      queue.pop_front();
      toggle = flip(toggle);
      return result;
    }
    if(not s.request)
    {
      return {.hs = usb::handshake::nak, .data = {}};
    }
    if(s.stalled)
    {
      return {.hs = usb::handshake::stall, .data = {}};
    }
    auto const& request = *s.request;
    if((request.request_type & 0x80) != 0 and request.length != 0 and not s.data_done)
    {
      if(s.silent)
      {
        return {.hs = usb::handshake::nak, .data = {}};
      }
      if(s.data_naks != 0)
      {
        --s.data_naks;
        return {.hs = usb::handshake::nak, .data = {}};
      }
      auto const     chunk = std::min<std::size_t>(64, s.reply.size() - s.sent);
      usb::in_result result{
          .hs   = usb::handshake::ack,
          .pid  = s.toggle,
          .data = {
                   s.reply.begin() + static_cast<std::ptrdiff_t>(s.sent),
                   s.reply.begin() + static_cast<std::ptrdiff_t>(s.sent + chunk)}
      };
      s.sent += chunk;
      s.toggle = flip(s.toggle);
      if(chunk < 64 or s.sent == request.length)
      {
        s.data_done = true;
      }
      return result;
    }
    if(s.next_address)
    {
      s.address = *s.next_address;
      s.next_address.reset();
    }
    return {.hs = usb::handshake::ack, .pid = usb::data_pid::data1, .data = {}};
  }
};

struct probe final : usb::class_driver
{
  std::function<void(usb::host_context&, probe&)>                on_configured{};
  std::function<void(usb::host_context&, probe&, std::uint16_t)> on_frame{};
  std::function<void(usb::host_context&, probe&, usb::pipe_id, usb::transfer_result const&)>
                                                             on_transfer{};
  std::vector<usb::transfer_result>                          controls{};
  std::vector<std::pair<usb::pipe_id, usb::transfer_result>> transfers{};
  std::vector<usb::pipe_id>                                  pipes{};
  std::vector<std::byte>                                     memory{};
  std::size_t                                                loads{};

  void configured(usb::host_context& ctx) override
  {
    if(on_configured)
    {
      on_configured(ctx, *this);
    }
  }

  void control_done(usb::host_context&, usb::transfer_result const& result) override
  {
    controls.push_back(result);
  }

  void transfer_done(usb::host_context& ctx, usb::pipe_id pipe, usb::transfer_result const& result)
      override
  {
    transfers.emplace_back(pipe, result);
    if(on_transfer)
    {
      on_transfer(ctx, *this, pipe, result);
    }
  }

  void frame(usb::host_context& ctx, std::uint16_t number) override
  {
    if(on_frame)
    {
      on_frame(ctx, *this, number);
    }
  }

  void save(std::vector<std::byte>& out) const override
  {
    out.insert(out.end(), memory.begin(), memory.end());
  }

  void load(std::span<std::byte const> in) override
  {
    memory.assign(in.begin(), in.end());
    ++loads;
  }
};

auto probe_factory(
    std::string                                     name,
    std::uint8_t                                    class_code,
    std::function<void(usb::host_context&, probe&)> on_configured = {}) -> usb::driver_factory
{
  return {
      .name  = std::move(name),
      .claim = [class_code](
                   usb::configuration_info const& config,
                   usb::interface_info const&     interface) -> std::vector<std::uint8_t> {
        if(interface.class_code != class_code)
        {
          return {};
        }
        return config.function_of(interface.number);
      },
      .make = [on_configured]() -> std::unique_ptr<usb::class_driver> {
        auto driver           = std::make_unique<probe>();
        driver->on_configured = on_configured;
        return driver;
      }};
}

struct rig
{
  fake_device                dev;
  std::vector<std::string>   warnings;
  std::shared_ptr<usb::host> host;

  explicit rig(usb::host_options options = {})
  {
    options.warn = [this](std::string const& message) { warnings.push_back(message); };
    host         = std::make_shared<usb::host>(dev, std::move(options));
  }

  rig(rig const&)                    = delete;
  auto operator=(rig const&) -> rig& = delete;

  void plug(std::uint64_t at = t0) const
  {
    host->pull_up_changed(true, vtime{at});
  }

  void run_until(std::uint64_t end)
  {
    while(dev.s.wake and *dev.s.wake <= end)
    {
      auto const at = *dev.s.wake;
      dev.s.wake.reset();
      host->on_time(vtime{at});
    }
  }

  [[nodiscard]] auto driver(std::string_view name) const -> probe*
  {
    for(auto const& bound : host->drivers())
    {
      if(bound.name == name)
      {
        return dynamic_cast<probe*>(bound.driver.get());
      }
    }
    return nullptr;
  }

  [[nodiscard]] auto driver_index(std::string_view name) const -> std::size_t
  {
    auto const drivers = host->drivers();
    auto const found   = std::ranges::find(drivers, name, &usb::bound_driver::name);
    REQUIRE(found != drivers.end());
    return static_cast<std::size_t>(found - drivers.begin());
  }
};

auto descriptor(std::uint8_t type, std::uint8_t index, std::uint16_t language, std::uint16_t length)
    -> usb::setup_packet
{
  return {
      .request_type = 0x80,
      .request      = 6,
      .value        = static_cast<std::uint16_t>((type << 8) | index),
      .index        = language,
      .length       = length};
}

auto expected_requests() -> std::vector<std::pair<std::uint8_t, usb::setup_packet>>
{
  return {
      {0, descriptor(1, 0, 0, 64)},
      {0, {.request_type = 0, .request = 5, .value = 1}},
      {1, descriptor(1, 0, 0, 18)},
      {1, descriptor(2, 0, 0, 9)},
      {1, descriptor(2, 0, 0, static_cast<std::uint16_t>(configuration_bytes().size()))},
      {1, descriptor(3, 0, 0, 255)},
      {1, descriptor(3, 1, 0x0409, 255)},
      {1, descriptor(3, 2, 0x0409, 255)},
      {1, descriptor(3, 3, 0x0409, 255)},
      {1, descriptor(3, 4, 0x0409, 255)},
      {1, descriptor(3, 5, 0x0409, 255)},
      {1, {.request_type = 0, .request = 9, .value = 1}}
  };
}

auto requests_of(device_script const& script)
    -> std::vector<std::pair<std::uint8_t, usb::setup_packet>>
{
  std::vector<std::pair<std::uint8_t, usb::setup_packet>> out;
  for(auto const& record : script.requests)
  {
    out.emplace_back(record.address, record.setup);
  }
  return out;
}
}

TEST_CASE("the configuration parser keeps interfaces, associations, endpoints and raw extras")
{
  auto const bytes  = configuration_bytes();
  auto const config = usb::parse_configuration(bytes);
  REQUIRE(config.has_value());
  CHECK(config->value == 1);
  CHECK(config->interface_count == 3);
  REQUIRE(config->interfaces.size() == 3);
  REQUIRE(config->associations.size() == 1);
  CHECK(config->associations[0].first_interface == 0);
  CHECK(config->associations[0].count == 2);
  auto const& comm = config->interfaces[0];
  CHECK(comm.class_code == 2);
  CHECK(comm.extra.size() == 19);
  REQUIRE(comm.endpoints.size() == 1);
  CHECK(comm.endpoints[0].address == 0x81);
  CHECK(comm.endpoints[0].type == usb::transfer_type::interrupt);
  CHECK(comm.endpoints[0].interval == 16);
  auto const& data = config->interfaces[1];
  REQUIRE(data.endpoints.size() == 2);
  CHECK(data.endpoints[0].type == usb::transfer_type::bulk);
  CHECK_FALSE(data.endpoints[0].in());
  CHECK(data.endpoints[1].in());
  CHECK(data.endpoints[1].number() == 2);
  auto const& hid = config->interfaces[2];
  CHECK(hid.extra.size() == 9);
  CHECK(hid.extra[1] == 0x21);
  CHECK(config->function_of(1) == std::vector<std::uint8_t>{0, 1});
  CHECK(config->function_of(2) == std::vector<std::uint8_t>{2});
  CHECK(config->find(2) == &hid);
  CHECK(config->find(2, 1) == nullptr);

  auto truncated = bytes;
  truncated.pop_back();
  CHECK_FALSE(usb::parse_configuration(truncated).has_value());
  auto broken = bytes;
  broken[9]   = 1;
  CHECK_FALSE(usb::parse_configuration(broken).has_value());

  auto const device = usb::parse_device_descriptor(device_bytes());
  REQUIRE(device.has_value());
  CHECK(device->vendor == 0x1209);
  CHECK(device->max_packet0 == 64);
  CHECK(device->serial_string == 3);
}

TEST_CASE("enumeration sends the Linux sequence, SET_CONFIGURATION 1 last, at the expected times")
{
  rig r;
  r.plug();
  CHECK(r.host->state() == usb::device_state::powered);
  r.run_until(t0 + second);
  CHECK(requests_of(r.dev.s) == expected_requests());
  CHECK(r.host->state() == usb::device_state::configured);
  CHECK(r.host->enumerated());
  CHECK_FALSE(r.host->failed_step().has_value());
  CHECK(r.host->address() == 1);
  CHECK(r.host->configuration_value() == 1);
  CHECK(r.dev.s.configuration == 1);
  CHECK(r.warnings.empty());
  CHECK(std::ranges::equal(r.host->device_bytes(), device_bytes()));
  CHECK(std::ranges::equal(r.host->configuration_bytes(), configuration_bytes()));
  REQUIRE(r.host->configuration() != nullptr);
  CHECK(r.host->language() == 0x0409);
  REQUIRE(r.host->strings().size() == 6);
  CHECK(r.host->strings()[2].bytes == utf16("composite"));

  REQUIRE(r.dev.s.resets.size() == 2);
  CHECK(r.dev.s.resets[0] == t0 + 100 * ms);
  auto const first_setup = r.dev.s.requests[0].at;
  CHECK(first_setup >= t0 + 110 * ms);
  CHECK(first_setup < t0 + 110 * ms + 60 * us);
  REQUIRE_FALSE(r.dev.s.frames.empty());
  CHECK(r.dev.s.frames[0] == t0 + 110 * ms);
  CHECK(r.dev.s.resets[1] > first_setup);
  CHECK(r.dev.s.requests[1].at >= r.dev.s.resets[1] + 10 * ms);
  CHECK(r.dev.s.requests[2].at >= r.dev.s.requests[1].at + 10 * ms);
  CHECK(std::ranges::none_of(r.dev.s.frames, [&](std::uint64_t at) {
    return at > r.dev.s.resets[1] and at < r.dev.s.resets[1] + 10 * ms;
  }));
  for(std::size_t n = 1; n < r.dev.s.frames.size(); ++n)
  {
    auto const gap = r.dev.s.frames[n] - r.dev.s.frames[n - 1];
    CHECK((gap == ms or gap >= 10 * ms));
  }

  auto const log = r.host->log();
  REQUIRE(log.size() == expected_requests().size() + 2);
  CHECK(log[0].kind == usb::log_kind::bus_reset);
  CHECK(log[1].kind == usb::log_kind::request);
  CHECK(log[1].at.ps == first_setup);
  CHECK(log[2].kind == usb::log_kind::bus_reset);
  CHECK(std::ranges::all_of(log | std::views::drop(3), [](usb::log_entry const& entry) {
    return entry.kind == usb::log_kind::request and entry.status == usb::transfer_status::completed;
  }));
}

TEST_CASE("a table without the second reset skips it")
{
  usb::host_options options;
  std::erase_if(options.enumeration, [](usb::enumeration_step const& step) {
    return step.action == usb::enumeration_action::bus_reset;
  });
  rig r{std::move(options)};
  r.plug();
  r.run_until(t0 + second);
  CHECK(r.dev.s.resets.size() == 1);
  CHECK(requests_of(r.dev.s) == expected_requests());
  CHECK(r.host->state() == usb::device_state::configured);
}

TEST_CASE("a data stage NAKed 50 times completes, one retry per frame, no busy loop")
{
  rig r;
  r.dev.s.data_naks = 50;
  r.plug();
  r.run_until(t0 + second);
  CHECK(r.host->state() == usb::device_state::configured);
  CHECK(requests_of(r.dev.s) == expected_requests());
  std::vector<std::uint64_t> naks;
  for(auto const& record : r.dev.s.tokens)
  {
    if(record.kind == token::in and record.endpoint == 0 and record.hs == usb::handshake::nak)
    {
      naks.push_back(record.at);
    }
  }
  REQUIRE(naks.size() == 50);
  auto const& frames = r.dev.s.frames;
  for(std::size_t n = 0; n + 1 < frames.size(); ++n)
  {
    auto const in_frame = [&](token_record const& record) {
      return record.at >= frames[n] and record.at < frames[n + 1];
    };
    CHECK(
        std::ranges::count_if(
            naks, [&](std::uint64_t at) { return at >= frames[n] and at < frames[n + 1]; })
        <= 1);
    CHECK(std::ranges::count_if(r.dev.s.tokens, in_frame) <= 100);
  }
  for(std::size_t n = 2; n < naks.size(); ++n)
  {
    CHECK(naks[n] - naks[n - 1] == ms);
  }
}

TEST_CASE("a device that never answers the data stage times out after 5 s, the host stops there")
{
  rig r;
  r.dev.s.silent = true;
  r.plug();
  r.run_until(t0 + 200 * ms);
  REQUIRE(r.dev.s.requests.size() == 1);
  auto const setup = r.dev.s.requests[0].at;
  r.run_until(setup + 5 * second - ms);
  CHECK(r.warnings.empty());
  CHECK_FALSE(r.host->failed_step().has_value());
  r.run_until(setup + 5 * second + ms);
  REQUIRE(r.warnings.size() == 1);
  CHECK(r.warnings[0].find("get_device_head") != std::string::npos);
  CHECK(r.warnings[0].find("timed_out") != std::string::npos);
  CHECK(r.host->failed_step() == usb::enumeration_action::get_device_head);
  CHECK(r.host->log().back().status == usb::transfer_status::timed_out);
  auto const frames = r.dev.s.frames.size();
  r.run_until(setup + 6 * second);
  CHECK(r.dev.s.requests.size() == 1);
  CHECK(r.dev.s.frames.size() > frames);
  CHECK(r.host->state() == usb::device_state::default_);
  CHECK_FALSE(r.host->enumerated());
}

TEST_CASE("a STALL on a string request skips that string and enumeration goes on")
{
  SUBCASE("one string")
  {
    rig r;
    r.dev.s.stalled_strings = {2};
    r.plug();
    r.run_until(t0 + second);
    CHECK(r.host->state() == usb::device_state::configured);
    CHECK(requests_of(r.dev.s) == expected_requests());
    std::vector<std::uint8_t> read;
    for(auto const& string : r.host->strings())
    {
      read.push_back(string.index);
    }
    CHECK(read == std::vector<std::uint8_t>{0, 1, 3, 4, 5});
    CHECK(r.warnings.empty());
  }
  SUBCASE("the language IDs")
  {
    rig r;
    r.dev.s.stalled_strings = {0};
    r.plug();
    r.run_until(t0 + second);
    CHECK(r.host->state() == usb::device_state::configured);
    CHECK(r.host->strings().empty());
    CHECK(r.host->language() == 0);
    CHECK(r.dev.s.requests.size() == 7);
    CHECK(r.dev.s.requests.back().setup.request == 9);
  }
}

TEST_CASE("a class request STALLed at configure reaches its driver, the device stays configured")
{
  usb::host_options options;
  options.drivers.push_back(probe_factory("acm", 2, [](usb::host_context& ctx, probe&) {
    auto const interface = ctx.interfaces()[0];
    ctx.control(
        {.request_type = 0x21, .request = 0x20, .index = interface, .length = 7},
        {0x00, 0xc2, 0x01, 0x00, 0, 0, 8});
    ctx.control({.request_type = 0x21, .request = 0x22, .value = 3, .index = interface});
  }));
  options.drivers.push_back(probe_factory("hid", 3));
  rig r{std::move(options)};
  r.dev.s.stall_class = true;
  r.plug();
  r.run_until(t0 + second);
  CHECK(r.host->state() == usb::device_state::configured);
  CHECK(r.warnings.empty());
  auto const drivers = r.host->drivers();
  REQUIRE(drivers.size() == 2);
  CHECK(drivers[0].name == "acm");
  CHECK(drivers[0].interfaces == std::vector<std::uint8_t>{0, 1});
  CHECK(drivers[1].name == "hid");
  CHECK(drivers[1].interfaces == std::vector<std::uint8_t>{2});
  auto* const acm = r.driver("acm");
  REQUIRE(acm != nullptr);
  REQUIRE(acm->controls.size() == 2);
  CHECK(acm->controls[0].status == usb::transfer_status::stalled);
  CHECK(acm->controls[0].setup.request == 0x20);
  CHECK(acm->controls[1].status == usb::transfer_status::stalled);
  CHECK(r.dev.s.requests.size() == expected_requests().size() + 2);

  r.dev.s.stall_class = false;
  r.host->context(r.driver_index("acm"))
      .control({.request_type = 0x21, .request = 0x22, .value = 1});
  r.run_until(t0 + second + 5 * ms);
  REQUIRE(acm->controls.size() == 3);
  CHECK(acm->controls[2].status == usb::transfer_status::completed);
}

TEST_CASE("a snapshot mid-enumeration restored on a fresh device gives the same remaining requests")
{
  for(auto const cut : {t0 + 105 * ms, t0 + 111 * ms, t0 + 115 * ms, t0 + 125 * ms, t0 + 130 * ms})
  {
    CAPTURE(cut);
    rig a;
    a.dev.s.data_naks = 3;
    a.plug();
    a.run_until(cut);
    std::vector<std::byte> bytes;
    a.host->save(bytes);
    auto const done = a.dev.s.requests.size();

    rig b;
    b.dev.s = a.dev.s;
    b.host->load(bytes);
    CHECK(b.host->state() == a.host->state());

    a.run_until(t0 + second);
    b.run_until(t0 + second);
    CHECK(a.host->state() == usb::device_state::configured);
    CHECK(b.host->state() == usb::device_state::configured);
    REQUIRE(a.dev.s.requests.size() == b.dev.s.requests.size());
    CHECK(a.dev.s.requests.size() > done);
    CHECK(a.dev.s.requests == b.dev.s.requests);
    CHECK(a.dev.s.frames == b.dev.s.frames);
    CHECK(std::ranges::equal(a.host->strings(), b.host->strings()));
  }
}

TEST_CASE("an empty snapshot gives a host that has seen no device")
{
  rig r;
  r.plug();
  r.run_until(t0 + second);
  r.host->load({});
  CHECK(r.host->state() == usb::device_state::detached);
  CHECK(r.host->device_bytes().empty());
  CHECK(r.host->drivers().empty());
}

TEST_CASE("drivers and their state come back from a snapshot through their factory")
{
  auto const options = [] {
    usb::host_options chosen;
    chosen.drivers.push_back(probe_factory("hid", 3, [](usb::host_context&, probe& self) {
      self.memory = {std::byte{7}, std::byte{9}};
    }));
    return chosen;
  };
  rig a{options()};
  a.plug();
  a.run_until(t0 + second);
  std::vector<std::byte> bytes;
  a.host->save(bytes);

  rig b{options()};
  b.dev.s = a.dev.s;
  b.host->load(bytes);
  auto* const restored = b.driver("hid");
  REQUIRE(restored != nullptr);
  CHECK(restored->loads == 1);
  CHECK(restored->memory == std::vector<std::byte>{std::byte{7}, std::byte{9}});
  CHECK(b.host->state() == usb::device_state::configured);

  auto* const kept = a.driver("hid");
  a.host->load(bytes);
  CHECK(a.driver("hid") == kept);
  CHECK(kept->loads == 1);
}

TEST_CASE("bulk OUT ends with a ZLP on a full last packet, bulk IN ends on a short packet")
{
  usb::host_options options;
  options.drivers.push_back(probe_factory("acm", 2, [](usb::host_context& ctx, probe& self) {
    auto const* const data = ctx.configuration().find(ctx.interfaces()[1]);
    REQUIRE(data != nullptr);
    for(auto const& endpoint : data->endpoints)
    {
      self.pipes.push_back(ctx.open_pipe(endpoint));
    }
    ctx.submit(self.pipes[0], {.data = std::vector<std::uint8_t>(128, 0x55), .zero_packet = true});
    ctx.submit(self.pipes[0], {.data = std::vector<std::uint8_t>(70, 0x66)});
    ctx.submit(self.pipes[1], {.length = 512});
  }));
  rig r{std::move(options)};
  r.dev.s.in_queue[2] = {std::vector<std::uint8_t>(64, 1), std::vector<std::uint8_t>(10, 2)};
  r.plug();
  r.run_until(t0 + second);
  auto* const acm = r.driver("acm");
  REQUIRE(acm != nullptr);
  auto const& received = r.dev.s.received[2];
  REQUIRE(received.size() == 5);
  CHECK(received[0].size() == 64);
  CHECK(received[1].size() == 64);
  CHECK(received[2].empty());
  CHECK(received[3].size() == 64);
  CHECK(received[4].size() == 6);
  REQUIRE(acm->transfers.size() == 3);
  auto const in = std::ranges::find(
      acm->transfers, acm->pipes[1], &std::pair<usb::pipe_id, usb::transfer_result>::first);
  REQUIRE(in != acm->transfers.end());
  CHECK(in->second.status == usb::transfer_status::completed);
  CHECK(in->second.data.size() == 74);
}

TEST_CASE("a NAKing bulk IN pipe resubmitted forever leaves room for control transfers")
{
  usb::host_options options;
  options.drivers.push_back(probe_factory("acm", 2, [](usb::host_context& ctx, probe& self) {
    auto const* const data = ctx.configuration().find(ctx.interfaces()[1]);
    self.pipes.push_back(ctx.open_pipe(data->endpoints[1]));
    ctx.submit(self.pipes[0], {.length = 64});
  }));
  rig r{std::move(options)};
  r.plug();
  r.run_until(t0 + second);
  auto const index  = r.driver_index("acm");
  auto*      acm    = r.driver("acm");
  acm->on_transfer  = [](usb::host_context& ctx, probe&, usb::pipe_id pipe,
                         usb::transfer_result const&) { ctx.submit(pipe, {.length = 64}); };
  auto const before = r.dev.s.tokens.size();
  r.run_until(t0 + second + 10 * ms);
  auto const polls = std::ranges::count_if(
      r.dev.s.tokens | std::views::drop(before),
      [](token_record const& record) { return record.endpoint == 2 and record.kind == token::in; });
  CHECK(polls == 10);
  r.host->context(index).control({.request_type = 0xa1, .request = 0x21, .length = 7});
  r.dev.s.in_queue[2] = {
      std::vector<std::uint8_t>{'h', 'i'}
  };
  r.run_until(t0 + second + 30 * ms);
  REQUIRE(acm->controls.size() == 1);
  CHECK(acm->controls[0].status == usb::transfer_status::completed);
  REQUIRE(acm->transfers.size() == 1);
  CHECK(acm->transfers[0].second.data == std::vector<std::uint8_t>{'h', 'i'});
  CHECK(r.host->drivers()[index].driver.get() == acm);
  CHECK(r.host->context(index).queued(acm->pipes[0]) == 1);
}

TEST_CASE("an interrupt IN pipe is polled every bInterval frames")
{
  usb::host_options options;
  options.drivers.push_back(probe_factory("hid", 3, [](usb::host_context& ctx, probe& self) {
    auto const* const hid = ctx.configuration().find(ctx.interfaces()[0]);
    self.pipes.push_back(ctx.open_pipe(hid->endpoints[0]));
    ctx.submit(self.pipes[0], {.length = 8});
  }));
  rig r{std::move(options)};
  r.plug();
  r.run_until(t0 + second);
  r.dev.s.in_queue[3] = {
      std::vector<std::uint8_t>{1, 0x42}
  };
  r.run_until(t0 + second + 50 * ms);
  std::vector<std::uint64_t> polls;
  for(auto const& record : r.dev.s.tokens)
  {
    if(record.endpoint == 3)
    {
      polls.push_back(record.at);
    }
  }
  REQUIRE(polls.size() >= 2);
  for(std::size_t n = 1; n < polls.size(); ++n)
  {
    CHECK(polls[n] - polls[n - 1] == 10 * ms);
  }
  auto* const hid = r.driver("hid");
  REQUIRE(hid->transfers.size() == 1);
  CHECK(hid->transfers[0].second.data == std::vector<std::uint8_t>{1, 0x42});
}

TEST_CASE("an iso pipe moves one packet per frame before EOPF, and closes synchronously")
{
  usb::host_options options;
  options.drivers.push_back(probe_factory("hid", 3, [](usb::host_context& ctx, probe& self) {
    self.pipes.push_back(ctx.open_pipe(
        {.address    = 0x84,
         .type       = usb::transfer_type::isochronous,
         .max_packet = 34,
         .interval   = 1}));
    self.on_frame = [](usb::host_context& frame_ctx, probe& driver, std::uint16_t) {
      if(not driver.pipes.empty() and frame_ctx.queued(driver.pipes[0]) == 0)
      {
        frame_ctx.submit(driver.pipes[0], {.length = 34});
      }
    };
  }));
  rig r{std::move(options)};
  r.dev.s.iso_endpoints = {4};
  r.plug();
  r.run_until(t0 + second);
  auto* const driver = r.driver("hid");
  REQUIRE(driver != nullptr);
  driver->transfers.clear();
  for(std::uint8_t n = 0; n < 5; ++n)
  {
    r.dev.s.in_queue[4].push_back(std::vector<std::uint8_t>(32, n));
  }
  auto const before = r.dev.s.tokens.size();
  auto const start  = *r.dev.s.wake;
  r.run_until(start + 8 * ms - 1);
  std::vector<token_record> iso;
  for(auto const& record : r.dev.s.tokens | std::views::drop(before))
  {
    if(record.endpoint == 4)
    {
      iso.push_back(record);
    }
  }
  REQUIRE(iso.size() == 8);
  for(auto const& record : iso)
  {
    auto const sof = *std::ranges::prev(std::ranges::upper_bound(r.dev.s.frames, record.at));
    CHECK(record.at - sof < 800 * us);
  }
  REQUIRE(driver->transfers.size() == 8);
  CHECK(driver->transfers[0].second.status == usb::transfer_status::completed);
  CHECK(driver->transfers[4].second.data == std::vector<std::uint8_t>(32, 4));
  CHECK(driver->transfers[5].second.status == usb::transfer_status::error);
  CHECK(driver->transfers[5].second.data.empty());

  auto ctx  = r.host->context(r.driver_index("hid"));
  auto pipe = driver->pipes[0];
  driver->pipes.clear();
  ctx.close_pipe(pipe);
  CHECK(ctx.queued(pipe) == 0);
  ctx.control({.request_type = 0x01, .request = 11, .value = 0, .index = 2});
  auto const requests = r.dev.s.requests.size();
  auto const after    = r.dev.s.tokens.size();
  r.run_until(start + 20 * ms);
  CHECK(
      std::ranges::none_of(
          r.dev.s.tokens | std::views::drop(after),
          [](token_record const& record) { return record.endpoint == 4; }));
  REQUIRE(r.dev.s.requests.size() == requests + 1);
  CHECK(r.dev.s.requests.back().setup.request == 11);
  REQUIRE(driver->controls.size() == 1);
  CHECK(driver->controls[0].status == usb::transfer_status::completed);
}

TEST_CASE("user control requests queue their results, a detach cancels the ones left")
{
  rig r;
  r.plug();
  r.run_until(t0 + second);
  auto const string0 = r.host->control(descriptor(3, 0, 0, 255));
  auto const unknown =
      r.host->control({.request_type = 0x82, .request = 0, .index = 0x85, .length = 2});
  CHECK(r.host->take_results().empty());
  r.run_until(t0 + second + 10 * ms);
  auto const results = r.host->take_results();
  REQUIRE(results.size() == 2);
  CHECK(results[0].id == string0);
  CHECK(results[0].status == usb::transfer_status::completed);
  CHECK(results[0].data == std::vector<std::uint8_t>{4, 3, 0x09, 0x04});
  CHECK(results[1].id == unknown);
  CHECK(results[1].status == usb::transfer_status::stalled);

  auto const pending = r.host->control(descriptor(1, 0, 0, 18));
  r.host->pull_up_changed(false, vtime{t0 + second + 11 * ms});
  CHECK(r.host->state() == usb::device_state::detached);
  auto const cancelled = r.host->take_results();
  REQUIRE(cancelled.size() == 1);
  CHECK(cancelled[0].id == pending);
  CHECK(cancelled[0].status == usb::transfer_status::cancelled);
  auto const late = r.host->control(descriptor(1, 0, 0, 18));
  auto const now  = r.host->take_results();
  REQUIRE(now.size() == 1);
  CHECK(now[0].id == late);
  CHECK(now[0].status == usb::transfer_status::cancelled);

  r.host->pull_up_changed(true, vtime{t0 + 2 * second});
  r.dev.s.requests.clear();
  r.run_until(t0 + 3 * second);
  CHECK(r.host->state() == usb::device_state::configured);
  CHECK(requests_of(r.dev.s) == expected_requests());
}
