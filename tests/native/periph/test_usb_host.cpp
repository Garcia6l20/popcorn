#include <popcorn/periph/usb.hpp>
#include <popcorn/periph/usb_cdc_acm.hpp>
#include <popcorn/periph/usb_host.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
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
  std::uint64_t                at{};
  token                        kind{};
  std::uint8_t                 endpoint{};
  usb::handshake               hs{};
  std::optional<usb::data_pid> pid{};
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
  std::uint32_t                                                  data_nones{};
  std::uint32_t                                                  setup_nones{};
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
  std::optional<std::vector<std::uint8_t>>                       config_bytes{};
  std::vector<std::uint8_t>                                      halted{};
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
    if(address != s.address or s.setup_nones != 0)
    {
      if(address == s.address)
      {
        --s.setup_nones;
      }
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
        s.reply = s.config_bytes.value_or(configuration_bytes());
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
    else if(packet.request_type == 2 and packet.request == 1 and packet.value == 0)
    {
      auto const endpoint = static_cast<std::uint8_t>(packet.index);
      std::erase(s.halted, endpoint);
      if((endpoint & 0x80u) != 0)
      {
        s.in_toggle[endpoint & 0x0fu] = usb::data_pid::data0;
      }
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
          usb::data_pid                 pid,
          vtime                         at) -> usb::handshake override
  {
    auto const hs = answer_out(address, endpoint, data);
    s.tokens.push_back({at.ps, token::out, endpoint, hs, pid});
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

  [[nodiscard]] auto now() const -> vtime override
  {
    return vtime{clock};
  }

  std::uint64_t clock{};

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
    if(std::ranges::contains(s.halted, endpoint))
    {
      return usb::handshake::stall;
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
      if(std::ranges::contains(s.halted, static_cast<std::uint8_t>(endpoint | 0x80u)))
      {
        return {.hs = usb::handshake::stall, .data = {}};
      }
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
      if(s.data_nones != 0)
      {
        --s.data_nones;
        return {};
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
  std::function<void()>                                      on_destroy{};

  ~probe() override
  {
    if(on_destroy)
    {
      on_destroy();
    }
  }

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
      dev.clock = at;
      host->on_time(vtime{at});
    }
    dev.clock = end;
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

auto fixed_claim(std::string name, std::uint8_t class_code, std::vector<std::uint8_t> claim)
    -> usb::driver_factory
{
  return {
      .name  = std::move(name),
      .claim = [class_code, claim](
                   usb::configuration_info const&,
                   usb::interface_info const& interface) -> std::vector<std::uint8_t> {
        return interface.class_code == class_code ? claim : std::vector<std::uint8_t>{};
      },
      .make = []() -> std::unique_ptr<usb::class_driver> { return std::make_unique<probe>(); }};
}

auto out_pids(device_script const& script, std::uint8_t endpoint) -> std::vector<usb::data_pid>
{
  std::vector<usb::data_pid> pids;
  for(auto const& record : script.tokens)
  {
    if(record.kind == token::out and record.endpoint == endpoint and record.pid)
    {
      pids.push_back(*record.pid);
    }
  }
  return pids;
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
  auto const composite = utf16("composite");
  CHECK(std::ranges::equal(r.host->strings()[2].bytes, std::as_bytes(std::span{composite})));

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
  using enum usb::data_pid;
  CHECK(out_pids(r.dev.s, 2) == std::vector{data0, data1, data0, data1, data0});
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

TEST_CASE("a restore drops the user results of the abandoned timeline")
{
  rig r;
  r.plug();
  r.run_until(t0 + second);
  std::vector<std::byte> bytes;
  r.host->save(bytes);
  auto const device    = r.dev.s;
  auto const abandoned = r.host->control(descriptor(3, 0, 0, 255));
  r.run_until(t0 + second + 10 * ms);

  r.host->load(bytes);
  r.dev.s = device;
  CHECK(r.host->take_results().empty());
  auto const fresh = r.host->control(descriptor(3, 0, 0, 255));
  CHECK(fresh == abandoned);
  r.run_until(t0 + second + 10 * ms);
  auto const results = r.host->take_results();
  REQUIRE(results.size() == 1);
  CHECK(results[0].id == fresh);
  CHECK(results[0].status == usb::transfer_status::completed);

  r.host->control(descriptor(1, 0, 0, 18));
  r.host->load({});
  CHECK(r.host->take_results().empty());
}

TEST_CASE("an empty load detaches at the device's current time, not at 0")
{
  rig r;
  r.plug();
  r.run_until(t0 + second);
  r.host->load({});
  CHECK(r.host->state() == usb::device_state::detached);
  r.host->control(descriptor(1, 0, 0, 18));
  auto const results = r.host->take_results();
  REQUIRE(results.size() == 1);
  CHECK(results[0].status == usb::transfer_status::cancelled);
  CHECK(results[0].at.ps == t0 + second);
}

TEST_CASE("a host snapshot keeps byte blobs as BSON binary, a 4 KiB driver state takes about 4 KiB")
{
  usb::host_options options;
  options.drivers.push_back(probe_factory("hid", 3, [](usb::host_context&, probe& self) {
    self.memory.assign(4096, std::byte{0x5a});
  }));
  rig r{std::move(options)};
  r.plug();
  r.run_until(t0 + second);
  std::vector<std::byte> empty_state;
  r.driver("hid")->memory.clear();
  r.host->save(empty_state);
  r.driver("hid")->memory.assign(4096, std::byte{0x5a});
  std::vector<std::byte> full_state;
  r.host->save(full_state);
  MESSAGE(
      "host state ", empty_state.size(), " B, with a 4096 B driver blob ", full_state.size(), " B");
  CHECK(full_state.size() - empty_state.size() == 4096);
  CHECK(empty_state.size() < 2048);
}

TEST_CASE(
    "a bus reset rewinds the control transfer in progress: error count, log and timeout start over")
{
  usb::host_options options;
  options.enumeration.insert(
      options.enumeration.begin() + 1,
      {.action = usb::enumeration_action::wait, .wait = std::chrono::milliseconds{20}});
  rig r{std::move(options)};
  r.plug();
  r.run_until(t0 + 115 * ms);
  REQUIRE(r.dev.s.requests.size() == 1);
  REQUIRE(r.dev.s.resets.size() == 1);
  r.dev.s.data_nones = 2;
  r.dev.s.data_naks  = 1'000'000;
  auto const id      = r.host->control(descriptor(1, 0, 0, 18));
  auto const before  = r.dev.s.tokens.size();
  r.run_until(t0 + 125 * ms);
  REQUIRE(r.dev.s.resets.size() == 1);
  CHECK(
      std::ranges::count_if(
          r.dev.s.tokens | std::views::drop(before),
          [](token_record const& record) { return record.hs == usb::handshake::none; })
      == 2);

  r.dev.s.setup_nones = 2;
  r.run_until(t0 + 200 * ms);
  REQUIRE(r.dev.s.resets.size() == 2);
  CHECK(r.dev.s.setup_nones == 0);
  CHECK(r.host->take_results().empty());
  REQUIRE(r.dev.s.requests.size() == 3);
  auto const first = r.dev.s.requests[1].at;
  CHECK(first < r.dev.s.resets[1]);
  std::vector<std::uint64_t> logged;
  for(auto const& entry : r.host->log())
  {
    if(entry.kind == usb::log_kind::request and entry.id == id)
    {
      logged.push_back(entry.at.ps);
    }
  }
  REQUIRE(logged.size() == 2);
  CHECK(logged[0] == first);
  auto const restart = logged[1];
  CHECK(restart > r.dev.s.resets[1]);
  CHECK(restart < r.dev.s.requests[2].at);

  r.run_until(first + 5 * second + 5 * ms);
  CHECK(r.host->take_results().empty());
  r.run_until(restart + 5 * second + 2 * ms);
  auto const results = r.host->take_results();
  REQUIRE(results.size() == 1);
  CHECK(results[0].id == id);
  CHECK(results[0].status == usb::transfer_status::timed_out);
  CHECK(results[0].at.ps >= restart + 5 * second);
}

TEST_CASE("a bus reset drops the control transfers of the drivers it unbinds")
{
  usb::host_options options;
  options.enumeration.push_back({.action = usb::enumeration_action::bus_reset});
  options.drivers.push_back(probe_factory("acm", 2, [](usb::host_context& ctx, probe&) {
    ctx.control({.request_type = 0xa1, .request = 0x21, .length = 7});
  }));
  rig r{std::move(options)};
  r.plug();
  r.run_until(t0 + second);
  REQUIRE(r.dev.s.resets.size() == 3);
  CHECK(r.host->drivers().empty());
  CHECK(r.host->state() == usb::device_state::default_);
  CHECK(std::ranges::none_of(r.dev.s.requests, [](request_record const& record) {
    return record.setup.request_type == 0xa1;
  }));
  auto const log   = r.host->log();
  auto const found = std::ranges::find_if(log, [](usb::log_entry const& entry) {
    return entry.kind == usb::log_kind::request and entry.setup.request_type == 0xa1;
  });
  CHECK(found == log.end());
}

TEST_CASE("a load destroys every driver it does not keep before it builds a replacement")
{
  auto events  = std::make_shared<std::vector<std::string>>();
  auto narrow  = std::make_shared<bool>(false);
  auto options = [&] {
    usb::host_options chosen;
    chosen.drivers.push_back(
        {.name  = "acm",
         .claim = [narrow](
                      usb::configuration_info const& config,
                      usb::interface_info const&     interface) -> std::vector<std::uint8_t> {
           if(interface.class_code != 2)
           {
             return {};
           }
           return *narrow ? std::vector<std::uint8_t>{interface.number}
                          : config.function_of(interface.number);
         },
         .make = [events]() -> std::unique_ptr<usb::class_driver> {
           events->push_back("make");
           auto driver        = std::make_unique<probe>();
           driver->on_destroy = [events] { events->push_back("destroy"); };
           return driver;
         }});
    return chosen;
  };
  rig a{options()};
  a.plug();
  a.run_until(t0 + second);
  std::vector<std::byte> bytes;
  a.host->save(bytes);

  *narrow = true;
  rig b{options()};
  b.plug();
  b.run_until(t0 + second);
  REQUIRE(b.host->drivers().size() == 1);
  CHECK(b.host->drivers()[0].interfaces == std::vector<std::uint8_t>{0});
  events->clear();
  b.dev.s = a.dev.s;
  b.host->load(bytes);
  CHECK(*events == std::vector<std::string>{"destroy", "make"});
  REQUIRE(b.host->drivers().size() == 1);
  CHECK(b.host->drivers()[0].interfaces == std::vector<std::uint8_t>{0, 1});
}

TEST_CASE("an iso packet that would end past EOPF ends in error in its own frame")
{
  usb::host_options options;
  options.drivers.push_back(probe_factory("hid", 3, [](usb::host_context& ctx, probe& self) {
    for(std::uint8_t const address : {0x84, 0x85})
    {
      self.pipes.push_back(ctx.open_pipe(
          {.address    = address,
           .type       = usb::transfer_type::isochronous,
           .max_packet = 512,
           .interval   = 1}));
    }
    self.on_frame = [](usb::host_context& frame_ctx, probe& driver, std::uint16_t) {
      for(auto const pipe : driver.pipes)
      {
        if(frame_ctx.queued(pipe) == 0)
        {
          frame_ctx.submit(pipe, {.length = 512});
        }
      }
    };
  }));
  rig r{std::move(options)};
  r.dev.s.iso_endpoints = {4, 5};
  r.plug();
  r.run_until(t0 + second);
  auto* const driver = r.driver("hid");
  REQUIRE(driver != nullptr);
  driver->transfers.clear();
  for(std::uint8_t n = 0; n < 10; ++n)
  {
    r.dev.s.in_queue[4].push_back(std::vector<std::uint8_t>(512, n));
  }
  auto const before = r.dev.s.tokens.size();
  r.run_until(t0 + second + 5 * ms);
  CHECK(
      std::ranges::none_of(
          r.dev.s.tokens | std::views::drop(before),
          [](token_record const& record) { return record.endpoint == 5; }));
  std::vector<std::uint16_t> sent;
  std::vector<std::uint16_t> dropped;
  for(auto const& [pipe, result] : driver->transfers)
  {
    if(pipe == driver->pipes[0])
    {
      CHECK(result.status == usb::transfer_status::completed);
      sent.push_back(result.frame);
    }
    else
    {
      CHECK(result.status == usb::transfer_status::error);
      CHECK(result.data.empty());
      dropped.push_back(result.frame);
    }
  }
  CHECK(sent.size() == 5);
  CHECK(dropped == sent);
}

TEST_CASE("a driver cannot close, submit on or reset the toggle of another driver's pipe")
{
  usb::host_options options;
  options.drivers.push_back(probe_factory("acm", 2, [](usb::host_context& ctx, probe& self) {
    auto const* const data = ctx.configuration().find(ctx.interfaces()[1]);
    self.pipes.push_back(ctx.open_pipe(data->endpoints[0]));
  }));
  options.drivers.push_back(probe_factory("hid", 3));
  rig r{std::move(options)};
  r.plug();
  r.run_until(t0 + second);
  auto* const acm  = r.driver("acm");
  auto const  pipe = acm->pipes[0];
  auto        mine = r.host->context(r.driver_index("acm"));
  auto        hers = r.host->context(r.driver_index("hid"));
  CHECK(
      mine.submit(
          pipe,
          {
              .data = {1, 2, 3}
  })
      != 0);
  r.run_until(t0 + second + 5 * ms);
  REQUIRE(r.warnings.empty());

  hers.close_pipe(pipe);
  CHECK(hers.submit(pipe, {.data = {9}}) == 0);
  hers.reset_toggle(pipe);
  REQUIRE(r.warnings.size() == 3);
  for(auto const& warning : r.warnings)
  {
    CHECK(warning.find("ignored") != std::string::npos);
  }
  CHECK(mine.queued(pipe) == 0);
  CHECK(mine.submit(pipe, {.data = {4}}) != 0);
  r.run_until(t0 + second + 10 * ms);
  REQUIRE(r.dev.s.received[2].size() == 2);
  CHECK(r.dev.s.received[2][1] == std::vector<std::uint8_t>{4});
  using enum usb::data_pid;
  CHECK(out_pids(r.dev.s, 2) == std::vector{data0, data1});
  CHECK(acm->transfers.size() == 2);
}

TEST_CASE("a claim naming a missing or taken interface binds nothing and the next factory is tried")
{
  usb::host_options options;
  options.drivers.push_back(probe_factory("acm", 2));
  options.drivers.push_back(fixed_claim("thief", 3, {2, 0}));
  options.drivers.push_back(fixed_claim("ghost", 3, {2, 7}));
  options.drivers.push_back(fixed_claim("twice", 3, {2, 2}));
  options.drivers.push_back(probe_factory("hid", 3));
  rig r{std::move(options)};
  r.plug();
  r.run_until(t0 + second);
  CHECK(r.host->state() == usb::device_state::configured);
  auto const drivers = r.host->drivers();
  REQUIRE(drivers.size() == 2);
  CHECK(drivers[0].name == "acm");
  CHECK(drivers[1].name == "hid");
  CHECK(drivers[1].interfaces == std::vector<std::uint8_t>{2});
  REQUIRE(r.warnings.size() == 3);
  CHECK(
      r.warnings[0].find("thief claims interface 0, which is already taken") != std::string::npos);
  CHECK(
      r.warnings[1].find("ghost claims interface 7, which is not in the configuration")
      != std::string::npos);
  CHECK(
      r.warnings[2].find("twice claims interface 2, which is already taken") != std::string::npos);
}

TEST_CASE("drivers are destroyed when the device detaches and when an empty snapshot loads")
{
  std::size_t       destroyed = 0;
  usb::host_options options;
  options.drivers.push_back(probe_factory(
      "hid", 3, [&](usb::host_context&, probe& self) { self.on_destroy = [&] { ++destroyed; }; }));
  rig r{std::move(options)};
  r.plug();
  r.run_until(t0 + second);
  REQUIRE(r.host->drivers().size() == 1);
  r.host->pull_up_changed(false, vtime{t0 + second});
  CHECK(destroyed == 1);
  CHECK(r.host->drivers().empty());

  r.plug(t0 + 2 * second);
  r.run_until(t0 + 3 * second);
  REQUIRE(r.host->drivers().size() == 1);
  r.host->load({});
  CHECK(destroyed == 2);
}

TEST_CASE(
    "reset_toggle starts a bulk pipe again at DATA0, and refuses the default or an unknown pipe")
{
  usb::host_options options;
  options.drivers.push_back(probe_factory("acm", 2, [](usb::host_context& ctx, probe& self) {
    auto const* const data = ctx.configuration().find(ctx.interfaces()[1]);
    self.pipes.push_back(ctx.open_pipe(data->endpoints[0]));
  }));
  rig r{std::move(options)};
  r.plug();
  r.run_until(t0 + second);
  auto const pipe = r.driver("acm")->pipes[0];
  auto       ctx  = r.host->context(r.driver_index("acm"));
  ctx.submit(pipe, {.data = {1}});
  r.run_until(t0 + second + 5 * ms);
  ctx.reset_toggle(pipe);
  ctx.submit(pipe, {.data = {2}});
  r.run_until(t0 + second + 10 * ms);
  ctx.submit(pipe, {.data = {3}});
  r.run_until(t0 + second + 15 * ms);
  using enum usb::data_pid;
  CHECK(out_pids(r.dev.s, 2) == std::vector{data0, data0, data1});
  CHECK(r.warnings.empty());

  ctx.reset_toggle(0);
  ctx.reset_toggle(pipe + 100);
  CHECK(r.warnings.size() == 2);
}

TEST_CASE("control OUT data packets run DATA1, DATA0, DATA1 and every status OUT is DATA1")
{
  rig r;
  r.plug();
  r.run_until(t0 + second);
  auto const statuses = out_pids(r.dev.s, 0);
  REQUIRE(statuses.size() == expected_requests().size() - 2);
  CHECK(
      std::ranges::all_of(statuses, [](usb::data_pid pid) { return pid == usb::data_pid::data1; }));

  r.dev.s.tokens.clear();
  std::vector<std::uint8_t> payload(130);
  for(std::size_t n = 0; n < payload.size(); ++n)
  {
    payload[n] = static_cast<std::uint8_t>(n);
  }
  auto const id = r.host->control({.request_type = 0x21, .request = 0x20, .length = 130}, payload);
  r.run_until(t0 + second + 10 * ms);
  auto const results = r.host->take_results();
  REQUIRE(results.size() == 1);
  CHECK(results[0].id == id);
  CHECK(results[0].status == usb::transfer_status::completed);
  CHECK(r.dev.s.class_out == payload);
  using enum usb::data_pid;
  CHECK(out_pids(r.dev.s, 0) == std::vector{data1, data0, data1});
  CHECK(r.dev.s.tokens.back().kind == token::in);
}

TEST_CASE("three missing handshakes in a row end a control data stage in error, two do not")
{
  rig r;
  r.plug();
  r.run_until(t0 + second);
  r.dev.s.data_nones = 2;
  r.host->control(descriptor(1, 0, 0, 18));
  r.run_until(t0 + second + 10 * ms);
  auto results = r.host->take_results();
  REQUIRE(results.size() == 1);
  CHECK(results[0].status == usb::transfer_status::completed);
  CHECK(results[0].data == device_bytes());

  r.dev.s.data_nones = 3;
  auto const before  = r.dev.s.tokens.size();
  r.host->control(descriptor(1, 0, 0, 18));
  r.run_until(t0 + second + 20 * ms);
  results = r.host->take_results();
  REQUIRE(results.size() == 1);
  CHECK(results[0].status == usb::transfer_status::error);
  CHECK(results[0].data.empty());
  CHECK(
      std::ranges::count_if(
          r.dev.s.tokens | std::views::drop(before),
          [](token_record const& record) {
            return record.kind == token::in and record.hs == usb::handshake::none;
          })
      == 3);
}

namespace
{
using packet_list = std::vector<std::vector<std::uint8_t>>;

auto cdc_options_with(std::shared_ptr<usb::cdc_hub> const& hub, usb::cdc_options cdc = {})
    -> usb::host_options
{
  usb::host_options options;
  options.drivers.push_back(usb::cdc_acm_factory(hub, cdc));
  return options;
}

auto two_acm_configuration() -> std::vector<std::uint8_t>
{
  std::vector<std::uint8_t> bytes{
      9, 2, 0, 0,    4, 1,    0, 0x80, 50,   9,    4,  0, 0,    1,  2,  2,    1, 0, 5, 0x24,
      6, 0, 1, 7,    5, 0x81, 3, 8,    0,    16,   9,  4, 1,    0,  2,  0x0a, 0, 0, 0, 7,
      5, 2, 2, 64,   0, 0,    7, 5,    0x82, 2,    64, 0, 0,    9,  4,  2,    0, 1, 2, 2,
      1, 0, 5, 0x24, 6, 2,    3, 7,    5,    0x83, 3,  8, 0,    16, 9,  4,    3, 0, 2, 0x0a,
      0, 0, 0, 7,    5, 4,    2, 64,   0,    0,    7,  5, 0x84, 2,  64, 0,    0};
  bytes[2] = static_cast<std::uint8_t>(bytes.size());
  bytes[3] = static_cast<std::uint8_t>(bytes.size() >> 8);
  return bytes;
}

auto class_requests(device_script const& script) -> std::vector<usb::setup_packet>
{
  std::vector<usb::setup_packet> out;
  for(auto const& record : script.requests)
  {
    if(((record.setup.request_type >> 5) & 3u) == 1)
    {
      out.push_back(record.setup);
    }
  }
  return out;
}

auto joined(std::span<std::vector<std::uint8_t> const> packets) -> std::vector<std::uint8_t>
{
  std::vector<std::uint8_t> out;
  for(auto const& packet : packets)
  {
    out.append_range(packet);
  }
  return out;
}

auto bytes_of(std::string_view text) -> std::vector<std::uint8_t>
{
  return {text.begin(), text.end()};
}

constexpr usb::setup_packet
    line_coding_request{.request_type = 0x21, .request = 0x20, .value = 0, .index = 0, .length = 7};

auto line_state_request(std::uint16_t lines, std::uint16_t comm = 0) -> usb::setup_packet
{
  return {.request_type = 0x21, .request = 0x22, .value = lines, .index = comm, .length = 0};
}
}

TEST_CASE("the CDC ACM driver binds through the association and sets 115200 8N1, then DTR and RTS")
{
  auto const hub = std::make_shared<usb::cdc_hub>();
  rig        r{cdc_options_with(hub)};
  r.plug();
  r.run_until(t0 + second);
  REQUIRE(r.host->drivers().size() == 1);
  CHECK(r.host->drivers()[0].name == usb::cdc_acm_name);
  CHECK(r.host->drivers()[0].interfaces == std::vector<std::uint8_t>{0, 1});
  CHECK(class_requests(r.dev.s) == std::vector{line_coding_request, line_state_request(3)});
  CHECK(r.dev.s.class_out == std::vector<std::uint8_t>{0x00, 0xc2, 0x01, 0x00, 0, 0, 8});
  auto const& channel = hub->channel(0);
  CHECK(channel.comm_interface == std::optional<std::uint8_t>{0});
  CHECK(channel.line_state == 3);
  CHECK(channel.coding == usb::line_coding{});
  CHECK(r.warnings.empty());
}

TEST_CASE("the CDC ACM driver leaves DTR off when its options say so")
{
  auto const hub = std::make_shared<usb::cdc_hub>();
  rig        r{cdc_options_with(hub, {.dtr = false})};
  r.plug();
  r.run_until(t0 + second);
  CHECK(class_requests(r.dev.s) == std::vector{line_coding_request, line_state_request(2)});
  CHECK(hub->channel(0).line_state == 2);
}

TEST_CASE("CDC OUT bytes go in packets of 64, with a ZLP after a full last one only")
{
  auto const hub = std::make_shared<usb::cdc_hub>();
  rig        r{cdc_options_with(hub)};
  r.plug();
  r.run_until(t0 + second);
  auto& channel = hub->channel(0);
  channel.out.append_range(std::vector<std::uint8_t>(64, 'a'));
  r.run_until(t0 + second + 10 * ms);
  auto const& received = r.dev.s.received[2];
  REQUIRE(received.size() == 2);
  CHECK(received[0].size() == 64);
  CHECK(received[1].empty());
  CHECK(channel.out.empty());
  CHECK(channel.in_flight == 0);

  std::vector<std::uint8_t> big(1000);
  for(std::size_t n = 0; n < big.size(); ++n)
  {
    big[n] = static_cast<std::uint8_t>(n * 7);
  }
  channel.out.append_range(big);
  r.run_until(t0 + second + 30 * ms);
  REQUIRE(received.size() == 2 + 16);
  CHECK(
      std::ranges::all_of(
          received | std::views::drop(2) | std::views::take(15),
          [](auto const& packet) { return packet.size() == 64; }));
  CHECK(received.back().size() == 40);
  CHECK(joined(std::span{received}.subspan(2)) == big);
}

TEST_CASE("CDC OUT bytes written before configuration wait for the control line state")
{
  auto const hub = std::make_shared<usb::cdc_hub>();
  hub->channel(0).out.append_range(bytes_of("early"));
  rig r{cdc_options_with(hub)};
  r.plug();
  r.run_until(t0 + second);
  CHECK(r.dev.s.received[2] == packet_list{bytes_of("early")});
  auto const line_state =
      std::ranges::find(r.dev.s.requests, line_state_request(3), &request_record::setup);
  REQUIRE(line_state != r.dev.s.requests.end());
  auto const first_out = std::ranges::find_if(r.dev.s.tokens, [](token_record const& record) {
    return record.kind == token::out and record.endpoint == 2;
  });
  REQUIRE(first_out != r.dev.s.tokens.end());
  CHECK(first_out->at > line_state->at);
}

TEST_CASE("CDC IN packets keep their time, a read splits a packet and keeps the rest")
{
  auto const hub = std::make_shared<usb::cdc_hub>();
  rig        r{cdc_options_with(hub)};
  r.plug();
  r.run_until(t0 + second);
  r.dev.s.in_queue[2] = {bytes_of("hi"), bytes_of("!")};
  r.run_until(t0 + second + 10 * ms);
  auto& channel = hub->channel(0);
  CHECK(channel.in_bytes == 3);
  CHECK(channel.packets() == 2);
  auto const first = channel.take(1);
  REQUIRE(first.size() == 1);
  CHECK(first[0].data == bytes_of("h"));
  auto const rest = channel.take_packets(5);
  REQUIRE(rest.size() == 2);
  CHECK(rest[0].data == bytes_of("i"));
  CHECK(rest[0].at == first[0].at);
  CHECK(rest[1].data == bytes_of("!"));
  CHECK(rest[1].at > rest[0].at);
  CHECK(channel.in_bytes == 0);

  channel.set_limit(2);
  r.dev.s.in_queue[2] = {bytes_of("abc")};
  r.run_until(t0 + second + 20 * ms);
  CHECK(channel.dropped == 1);
  CHECK(channel.take(10)[0].data == bytes_of("bc"));
}

TEST_CASE("a SERIAL_STATE notification split over two packets sets the serial state")
{
  auto const hub = std::make_shared<usb::cdc_hub>();
  rig        r{cdc_options_with(hub)};
  r.plug();
  r.run_until(t0 + second);
  r.dev.s.in_queue[1] = {
      std::vector<std::uint8_t>{0xa1, 0x20, 0, 0, 0, 0, 2, 0},
      std::vector<std::uint8_t>{0x03, 0x00}
  };
  r.run_until(t0 + second + 50 * ms);
  CHECK(hub->channel(0).serial_state == 3);
}

TEST_CASE("a control line state request through the driver reaches the device and its result")
{
  auto const hub = std::make_shared<usb::cdc_hub>();
  rig        r{cdc_options_with(hub)};
  r.plug();
  r.run_until(t0 + second);
  auto&      channel = hub->channel(0);
  auto const id      = r.host->context(0).control(line_state_request(0));
  channel.awaited.insert(id);
  r.run_until(t0 + second + 10 * ms);
  CHECK(channel.results.at(id) == usb::transfer_status::completed);
  CHECK(channel.awaited.empty());
  CHECK(channel.line_state == 0);
  CHECK(class_requests(r.dev.s).back() == line_state_request(0));
}

TEST_CASE("two ACM functions without associations bind through their union descriptors")
{
  auto const hub = std::make_shared<usb::cdc_hub>();
  rig        r{cdc_options_with(hub)};
  r.dev.s.config_bytes = two_acm_configuration();
  r.plug();
  r.run_until(t0 + second);
  REQUIRE(r.host->drivers().size() == 2);
  CHECK(r.host->drivers()[0].interfaces == std::vector<std::uint8_t>{0, 1});
  CHECK(r.host->drivers()[1].interfaces == std::vector<std::uint8_t>{2, 3});
  REQUIRE(r.host->configuration() != nullptr);
  CHECK(usb::cdc_acm_functions(*r.host->configuration()) == std::vector<std::uint8_t>{0, 2});
  CHECK(
      class_requests(r.dev.s)
      == std::vector{
          line_coding_request, line_state_request(3),
          usb::setup_packet{
                            .request_type = 0x21, .request = 0x20, .value = 0, .index = 2, .length = 7},
          line_state_request(3, 2)
  });
  CHECK(hub->channel(1).comm_interface == std::optional<std::uint8_t>{2});
  hub->channel(1).out.append_range(bytes_of("xy"));
  r.dev.s.in_queue[2] = {bytes_of("zero")};
  r.dev.s.in_queue[4] = {bytes_of("one")};
  r.run_until(t0 + second + 10 * ms);
  CHECK(r.dev.s.received[4] == packet_list{bytes_of("xy")});
  CHECK(r.dev.s.received[2].empty());
  CHECK(hub->channel(0).take(10)[0].data == bytes_of("zero"));
  CHECK(hub->channel(1).take(10)[0].data == bytes_of("one"));
}

TEST_CASE("a restore sends the OUT bytes the snapshot had pending and drops later IN bytes")
{
  auto const hub = std::make_shared<usb::cdc_hub>();
  rig        a{cdc_options_with(hub)};
  a.plug();
  a.run_until(t0 + second);
  auto& channel = hub->channel(0);
  channel.out.append_range(bytes_of("abc"));
  std::vector<std::byte> saved;
  a.host->save(saved);
  auto const device_at_save = a.dev.s;

  a.dev.s.in_queue[2] = {bytes_of("late")};
  a.run_until(t0 + second + 10 * ms);
  CHECK(a.dev.s.received[2] == packet_list{bytes_of("abc")});
  CHECK(channel.in_bytes == 4);

  auto* const kept = a.host->drivers()[0].driver.get();
  hub->discard();
  a.host->load(saved);
  CHECK(a.host->drivers()[0].driver.get() == kept);
  CHECK(channel.in_bytes == 0);
  CHECK(std::ranges::equal(channel.out, bytes_of("abc")));
  CHECK(channel.comm_interface == std::optional<std::uint8_t>{0});
  CHECK(channel.line_state == 3);
  a.run_until(t0 + second + 20 * ms);
  CHECK(a.dev.s.received[2] == packet_list{bytes_of("abc"), bytes_of("abc")});

  auto const fresh = std::make_shared<usb::cdc_hub>();
  rig        b{cdc_options_with(fresh)};
  b.dev.s = device_at_save;
  b.host->load(saved);
  CHECK(std::ranges::equal(fresh->channel(0).out, bytes_of("abc")));
  CHECK(fresh->channel(0).comm_interface == std::optional<std::uint8_t>{0});
  b.run_until(t0 + second + 10 * ms);
  CHECK(b.dev.s.received[2] == packet_list{bytes_of("abc")});
}

TEST_CASE("a snapshot in the middle of an OUT transfer restores it in the middle")
{
  auto const hub = std::make_shared<usb::cdc_hub>();
  rig        a{cdc_options_with(hub)};
  a.plug();
  a.run_until(t0 + second);
  std::vector<std::uint8_t> big(1000);
  for(std::size_t n = 0; n < big.size(); ++n)
  {
    big[n] = static_cast<std::uint8_t>(n * 13);
  }
  hub->channel(0).out.append_range(big);
  auto end = t0 + second;
  while(a.dev.s.received[2].size() < 4)
  {
    end += 20 * us;
    a.run_until(end);
  }
  CHECK(hub->channel(0).in_flight == 1000);
  std::vector<std::byte> saved;
  a.host->save(saved);
  auto const device_at_save = a.dev.s;
  a.run_until(end + 10 * ms);
  CHECK(joined(a.dev.s.received[2]) == big);

  auto const fresh = std::make_shared<usb::cdc_hub>();
  rig        b{cdc_options_with(fresh)};
  b.dev.s     = device_at_save;
  b.dev.clock = end;
  b.host->load(saved);
  CHECK(fresh->channel(0).in_flight == 1000);
  CHECK(fresh->channel(0).out.empty());
  b.run_until(end + 10 * ms);
  CHECK(joined(b.dev.s.received[2]) == big);
  CHECK(fresh->channel(0).in_flight == 0);
  CHECK(b.dev.s.received[2] == a.dev.s.received[2]);
}

TEST_CASE("an unplug unbinds the CDC channel and keeps it, a replug binds the same channel again")
{
  auto const hub = std::make_shared<usb::cdc_hub>();
  rig        r{cdc_options_with(hub)};
  r.plug();
  r.run_until(t0 + second);
  r.dev.s.in_queue[2] = {bytes_of("a")};
  r.run_until(t0 + second + 10 * ms);
  auto& channel = hub->channel(0);
  r.host->pull_up_changed(false, vtime{t0 + second + 10 * ms});
  CHECK(r.host->drivers().empty());
  CHECK_FALSE(channel.comm_interface.has_value());
  CHECK(channel.take(5)[0].data == bytes_of("a"));
  CHECK(channel.take(5).empty());
  channel.out.append_range(bytes_of("zz"));
  r.run_until(t0 + second + 50 * ms);
  CHECK(r.dev.s.received[2].empty());

  r.plug(t0 + second + 50 * ms);
  r.run_until(t0 + 3 * second);
  CHECK(r.host->state() == usb::device_state::configured);
  CHECK(channel.comm_interface == std::optional<std::uint8_t>{0});
  CHECK(r.dev.s.received[2] == packet_list{bytes_of("zz")});
  CHECK(&hub->channel(0) == &channel);
}

TEST_CASE("a bulk IN STALL is cleared as Linux does and reading carries on at DATA0")
{
  auto const hub = std::make_shared<usb::cdc_hub>();
  rig        r{cdc_options_with(hub)};
  r.plug();
  r.run_until(t0 + second);
  r.dev.s.in_queue[2] = {bytes_of("a")};
  r.run_until(t0 + second + 5 * ms);
  r.dev.s.halted = {0x82};
  r.run_until(t0 + second + 10 * ms);
  CHECK(r.dev.s.halted.empty());
  auto const clear = std::ranges::find(
      r.dev.s.requests,
      usb::setup_packet{.request_type = 2, .request = 1, .value = 0, .index = 0x82, .length = 0},
      &request_record::setup);
  REQUIRE(clear != r.dev.s.requests.end());
  r.dev.s.in_queue[2] = {bytes_of("b"), bytes_of("c")};
  r.run_until(t0 + second + 20 * ms);
  auto const read = hub->channel(0).take(10);
  REQUIRE(read.size() == 3);
  CHECK(read[1].data == bytes_of("b"));
  CHECK(read[2].data == bytes_of("c"));
  CHECK(read[1].at.ps > clear->at);
  CHECK(r.warnings.empty());
}

TEST_CASE("a bulk OUT STALL stops writing with a warning and sends no request")
{
  auto const hub = std::make_shared<usb::cdc_hub>();
  rig        r{cdc_options_with(hub)};
  r.plug();
  r.run_until(t0 + second);
  auto const requests = r.dev.s.requests.size();
  r.dev.s.halted      = {0x02};
  auto& channel       = hub->channel(0);
  channel.out.append_range(bytes_of("lost"));
  r.run_until(t0 + second + 10 * ms);
  REQUIRE(r.warnings.size() == 1);
  CHECK(
      r.warnings[0].find("bulk OUT ended stalled, 4 bytes lost, writing stops")
      != std::string::npos);
  CHECK(channel.in_flight == 0);
  r.dev.s.halted.clear();
  channel.out.append_range(bytes_of("next"));
  r.run_until(t0 + second + 20 * ms);
  CHECK(r.dev.s.received[2].empty());
  CHECK(std::ranges::equal(channel.out, bytes_of("next")));
  CHECK(r.dev.s.requests.size() == requests);
}

TEST_CASE("a notification cut short by a short packet is dropped, the next one parses")
{
  auto const hub = std::make_shared<usb::cdc_hub>();
  rig        r{cdc_options_with(hub)};
  r.plug();
  r.run_until(t0 + second);
  r.dev.s.in_queue[1] = {
      std::vector<std::uint8_t>{0xa1, 0x20, 0, 0, 0, 0, 2, 0},
      std::vector<std::uint8_t>{},
      std::vector<std::uint8_t>{0xa1, 0x20, 0, 0, 0, 0, 2, 0},
      std::vector<std::uint8_t>{0x05, 0x00}
  };
  r.run_until(t0 + second + 100 * ms);
  CHECK(r.dev.s.in_queue[1].empty());
  CHECK(hub->channel(0).serial_state == 5);
}

TEST_CASE("OUT bytes of an unbound function are saved with the host and sent after the restore")
{
  auto const hub = std::make_shared<usb::cdc_hub>();
  rig        a{cdc_options_with(hub)};
  hub->channel(0).out.append_range(bytes_of("early"));
  std::vector<std::byte> saved;
  a.host->save(saved);
  hub->channel(0).out.clear();

  auto const fresh = std::make_shared<usb::cdc_hub>();
  rig        b{cdc_options_with(fresh)};
  fresh->channel(0).out.append_range(bytes_of("stale"));
  b.host->load(saved);
  CHECK(std::ranges::equal(fresh->channel(0).out, bytes_of("early")));
  b.plug();
  b.run_until(t0 + second);
  CHECK(b.dev.s.received[2] == packet_list{bytes_of("early")});

  std::vector<std::byte> empty_hub;
  rig{cdc_options_with(hub)}.host->save(empty_hub);
  b.host->load(empty_hub);
  CHECK(fresh->channel(0).out.empty());
}
