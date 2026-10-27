#include "stm32_fixture.hpp"

#include <popcorn/core/machine.hpp>
#include <popcorn/core/snapshot.hpp>
#include <popcorn/periph/stm32/board.hpp>
#include <popcorn/periph/stm32/gpio.hpp>
#include <popcorn/periph/stm32/usb.hpp>
#include <popcorn/periph/usb.hpp>
#include <popcorn/periph/usb_host.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
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
namespace core  = popcorn::core;
namespace stm32 = popcorn::periph::stm32;
namespace usb   = popcorn::periph::usb;
using popcorn::icount;
using popcorn::stop_reason;
using popcorn::vtime;
using popcorn::testing::stm32_fixture;
using popcorn::testing::u575;

constexpr std::uint64_t ms = 1'000'000'000;
constexpr std::uint64_t us = 1'000'000;

constexpr usb::setup_packet
    get_device{.request_type = 0x80, .request = 6, .value = 0x0100, .index = 0, .length = 18};
constexpr usb::setup_packet
    set_line_state{.request_type = 0x21, .request = 0x22, .value = 3, .index = 0, .length = 0};

struct traffic
{
  std::vector<std::uint8_t>         received;
  std::vector<std::vector<uint8_t>> reports;
  std::vector<usb::transfer_status> controls;
  std::vector<std::uint8_t>         to_send;
  std::size_t                       sends{};
};

class echo_driver final : public usb::class_driver
{
public:
  explicit echo_driver(std::shared_ptr<traffic> t) : traffic_{std::move(t)}
  {}

  void configured(usb::host_context& ctx) override
  {
    for(auto const& setting : ctx.configuration().interfaces)
    {
      for(auto const& endpoint : setting.endpoints)
      {
        if(endpoint.type == usb::transfer_type::bulk and endpoint.in())
        {
          bulk_in_ = ctx.open_pipe(endpoint);
        }
        else if(endpoint.type == usb::transfer_type::bulk)
        {
          bulk_out_ = ctx.open_pipe(endpoint);
        }
        else if(
            endpoint.type == usb::transfer_type::interrupt
            and endpoint.in()
            and setting.class_code == 3
            and report_ == 0)
        {
          report_ = ctx.open_pipe(endpoint);
        }
      }
    }
    ctx.submit(bulk_in_, {.length = 64});
    ctx.submit(report_, {.length = 16});
    ctx.control(set_line_state);
  }

  void control_done(usb::host_context& ctx, usb::transfer_result const& result) override
  {
    traffic_->controls.push_back(result.status);
    send(ctx);
  }

  void transfer_done(usb::host_context& ctx, usb::pipe_id pipe, usb::transfer_result const& result)
      override
  {
    if(pipe == bulk_in_)
    {
      traffic_->received.insert(traffic_->received.end(), result.data.begin(), result.data.end());
      ctx.submit(bulk_in_, {.length = 64});
    }
    else if(pipe == report_)
    {
      traffic_->reports.push_back(result.data);
      ctx.submit(report_, {.length = 16});
    }
  }

  void frame(usb::host_context& ctx, std::uint16_t) override
  {
    send(ctx);
  }

private:
  void send(usb::host_context& ctx)
  {
    if(not traffic_->to_send.empty() and ctx.queued(bulk_out_) == 0)
    {
      ++traffic_->sends;
      ctx.submit(
          bulk_out_,
          {.data = std::exchange(traffic_->to_send, {}), .length = 0, .zero_packet = true});
    }
  }

  std::shared_ptr<traffic> traffic_;
  usb::pipe_id             bulk_in_{};
  usb::pipe_id             bulk_out_{};
  usb::pipe_id             report_{};
};

auto board_with_host(std::shared_ptr<traffic> const& t, bool attached) -> stm32::board
{
  stm32::board            board;
  stm32::usb_host_options options{.attached = attached};
  if(t)
  {
    options.host.drivers.push_back(
        {.name = "echo",
         .claim =
             [](usb::configuration_info const& configuration, usb::interface_info const&) {
               return configuration.interfaces
                    | std::views::transform(&usb::interface_info::number)
                    | std::ranges::to<std::vector>();
             },
         .make = [t] { return std::make_unique<echo_driver>(t); }});
  }
  board.usb_hosts.emplace("OTG_FS", std::move(options));
  return board;
}

struct rig
{
  std::shared_ptr<traffic>                  t      = std::make_shared<traffic>();
  std::shared_ptr<std::vector<std::string>> logged = std::make_shared<std::vector<std::string>>();
  std::unique_ptr<stm32_fixture>            f;
  core::hook_handle                         sink;

  explicit rig(bool with_driver = true, bool attached = true)
  {
    auto options   = popcorn::testing::stm32_options();
    auto factories = stm32::factories(board_with_host(with_driver ? t : nullptr, attached), u575());
    options.models.emplace("OTG_FS", std::move(factories.at("OTG_FS")));
    options.spin_threshold     = 0;
    options.strict_peripherals = true;
    f    = std::make_unique<stm32_fixture>(u575(), "usb_cdc", std::move(options));
    sink = f->target->add_log_sink(
        [logged = logged](core::log_record const& record) { logged->push_back(record.message); });
  }

  [[nodiscard]] auto port() const -> stm32::usb_host_port&
  {
    auto* const found = f->model_of<stm32::usb_host_port>("OTG_FS");
    REQUIRE(found != nullptr);
    return *found;
  }

  [[nodiscard]] auto host() const -> usb::host&
  {
    auto* const found = port().board_host();
    REQUIRE(found != nullptr);
    return *found;
  }

  [[nodiscard]] auto now() const -> vtime
  {
    return f->target->virtual_clock().time();
  }

  void run_for(std::uint64_t ps) const
  {
    REQUIRE(
        f->run_to_stop(
             {.max_instructions = icount{4'000'000'000}, .until_time = vtime{now().ps + ps}})
            .reason
        == stop_reason::time);
  }

  auto control(usb::setup_packet setup) const -> usb::transfer_result
  {
    auto const id = host().control(setup);
    for(int frames = 0; frames < 100; ++frames)
    {
      run_for(1 * ms);
      for(auto& result : host().take_results())
      {
        if(result.id == id)
        {
          return result;
        }
      }
    }
    FAIL("no result");
    return {};
  }

  [[nodiscard]] auto var(std::string_view name) const -> std::uint32_t
  {
    auto const value = f->target->mem().read(popcorn::testing::sym("usb_cdc", name), 4);
    REQUIRE(value.has_value());
    return static_cast<std::uint32_t>(*value);
  }

  void restore_from(core::snapshot const& saved) const
  {
    f->target->restore(core::snapshot{core::decode_snapshot(core::encode_snapshot(saved.data()))});
  }

  [[nodiscard]] auto configured_at(std::uint64_t step, std::uint64_t until) const -> std::uint64_t
  {
    while(now().ps < until)
    {
      if(host().state() == usb::device_state::configured)
      {
        return now().ps;
      }
      run_for(step);
    }
    FAIL("not configured");
    return 0;
  }

  void press(bool level) const
  {
    auto* const gpio = f->model_of<stm32::gpio_port>("GPIOC");
    REQUIRE(gpio != nullptr);
    gpio->drive(13, level);
  }
};

TEST_CASE("the board host enumerates the TinyUSB firmware from power-on")
{
  rig const r{false};
  CHECK(r.port().plugged());
  r.run_for(300 * ms);
  CHECK(r.host().state() == usb::device_state::configured);
  CHECK(r.host().address() == 1);
  CHECK_FALSE(r.host().failed_step().has_value());
  REQUIRE(r.host().configuration() != nullptr);
  CHECK(r.host().configuration()->interfaces.size() == 4);
}

TEST_CASE("an unplugged board host sees nothing until the cable is plugged")
{
  rig const r{false, false};
  CHECK_FALSE(r.port().plugged());
  r.run_for(300 * ms);
  CHECK(r.host().state() == usb::device_state::detached);
  r.port().plug(true);
  r.run_for(300 * ms);
  CHECK(r.host().state() == usb::device_state::configured);
  r.port().plug(false);
  CHECK(r.host().state() == usb::device_state::detached);
  CHECK(r.host().drivers().empty());
}

TEST_CASE("a power-on reset plugs the cable back as the board says, other resets keep it")
{
  rig const r{false};
  r.port().plug(false);
  r.f->target->reset(popcorn::reset_kind::software);
  CHECK_FALSE(r.port().plugged());
  r.f->target->reset(popcorn::reset_kind::power_on);
  CHECK(r.port().plugged());
}

TEST_CASE("a snapshot of a configured device restores configured, a request works at once")
{
  rig const r{false};
  r.run_for(300 * ms);
  REQUIRE(r.host().state() == usb::device_state::configured);
  auto const saved = r.f->target->snapshot();

  rig const other{false, false};
  other.f->target->restore(
      core::snapshot{core::decode_snapshot(core::encode_snapshot(saved.data()))});
  CHECK(other.port().plugged());
  CHECK(other.host().state() == usb::device_state::configured);
  other.host().clear_log();
  auto const result = other.control(get_device);
  CHECK(result.status == usb::transfer_status::completed);
  CHECK(result.data.size() == 18);
  CHECK(other.host().log().size() == 1);
}

TEST_CASE("a snapshot with the cable unplugged restores it unplugged and the host detached")
{
  rig const r{false};
  r.run_for(300 * ms);
  r.port().plug(false);
  auto const saved = r.f->target->snapshot();

  rig const other{false};
  other.run_for(300 * ms);
  REQUIRE(other.host().state() == usb::device_state::configured);
  other.f->target->restore(
      core::snapshot{core::decode_snapshot(core::encode_snapshot(saved.data()))});
  CHECK_FALSE(other.port().plugged());
  CHECK(other.host().state() == usb::device_state::detached);
}

struct counting_host final : usb::host_side
{
  mutable std::size_t saves{};

  void pull_up_changed(bool, vtime) override
  {}

  void on_time(vtime) override
  {}

  void save(std::vector<std::byte>&) const override
  {
    ++saves;
  }

  void load(std::span<std::byte const>) override
  {}
};

TEST_CASE("unplugging a connected device suspends it 3 ms later, replugging enumerates again")
{
  rig const r{false};
  r.run_for(300 * ms);
  REQUIRE(r.host().state() == usb::device_state::configured);
  REQUIRE(r.var("usb_suspends") == 0);
  r.port().plug(false);
  r.run_for(2 * ms);
  CHECK(r.var("usb_suspends") == 0);
  CHECK(r.f->field("OTG_FS", "DSTS", "SUSPSTS") == 0);
  r.run_for(2 * ms);
  CHECK(r.var("usb_suspends") == 1);
  CHECK(r.f->field("OTG_FS", "DSTS", "SUSPSTS") == 1);
  r.port().plug(true);
  r.run_for(300 * ms);
  CHECK(r.host().state() == usb::device_state::configured);
  CHECK(r.f->field("OTG_FS", "DSTS", "SUSPSTS") == 0);
  CHECK(r.var("usb_suspends") == 1);
  CHECK(*r.logged == std::vector<std::string>{});
}

TEST_CASE("a cable plugged back within 3 ms cancels the suspend")
{
  rig const r{false};
  r.run_for(300 * ms);
  r.port().plug(false);
  r.run_for(1 * ms);
  r.port().plug(true);
  r.run_for(10 * ms);
  CHECK(r.var("usb_suspends") == 0);
  CHECK(r.f->field("OTG_FS", "DSTS", "SUSPSTS") == 0);
}

TEST_CASE("a cable plugged back, a soft disconnect and an unplug within 3 ms never suspend")
{
  rig const r{false};
  r.run_for(300 * ms);
  r.port().plug(false);
  r.run_for(1 * ms);
  r.port().plug(true);
  r.run_for(500 * us);
  auto const& desc = r.f->target->peripheral("OTG_FS").description();
  auto const* dctl = popcorn::find_register(desc, "DCTL");
  REQUIRE(dctl != nullptr);
  auto const sdis =
      std::ranges::find(dctl->fields, std::string_view{"SDIS"}, &popcorn::field_desc::name);
  REQUIRE(sdis != dctl->fields.end());
  auto const at  = desc.base + dctl->offset;
  auto const old = r.f->target->mem().read(at, 4);
  REQUIRE(old.has_value());
  REQUIRE(r.f->target->mem().write(at, 4, *old | (std::uint32_t{1} << sdis->lsb)));
  REQUIRE_FALSE(dynamic_cast<usb::device_port&>(r.port()).pulled_up());
  r.port().plug(false);
  r.run_for(4 * ms);
  CHECK(r.var("usb_suspends") == 0);
  CHECK(r.f->field("OTG_FS", "DSTS", "SUSPSTS") == 0);
}

TEST_CASE("with a board host, a host given to attach() is not saved")
{
  rig const r{false};
  r.run_for(300 * ms);
  auto const other = std::make_shared<counting_host>();
  dynamic_cast<usb::device_port&>(r.port()).attach(other);
  std::ignore = r.f->target->snapshot();
  CHECK(other->saves == 0);
  r.port().plug(true);
  CHECK(r.port().board_host() == &r.host());
}

TEST_CASE("a snapshot inside the enumeration restores into a fresh machine on the same timeline")
{
  constexpr auto cut  = 120 * ms;
  constexpr auto step = 50 * us;
  rig const      whole{false};
  whole.run_for(cut);
  auto const state = whole.host().state();
  REQUIRE(state != usb::device_state::detached);
  REQUIRE(state != usb::device_state::configured);
  auto const saved = whole.f->target->snapshot();
  whole.host().clear_log();
  auto const expected = whole.configured_at(step, 300 * ms);
  whole.run_for(1 * ms);

  rig const other{false, false};
  other.restore_from(saved);
  CHECK(other.now().ps == cut);
  CHECK(other.host().state() == state);
  CHECK(other.configured_at(step, 300 * ms) == expected);
  other.run_for(1 * ms);

  auto const requests = [](rig const& of) {
    std::vector<std::pair<std::uint64_t, usb::setup_packet>> out;
    for(auto const& entry : of.host().log())
    {
      out.emplace_back(entry.at.ps, entry.setup);
    }
    return out;
  };
  REQUIRE_FALSE(requests(whole).empty());
  CHECK(requests(other) == requests(whole));
}

TEST_CASE("a user request queued at a snapshot completes after the restore")
{
  rig const r{false};
  r.run_for(300 * ms);
  auto const id    = r.host().control(get_device);
  auto const saved = r.f->target->snapshot();

  rig const other{false, false};
  other.restore_from(saved);
  other.run_for(5 * ms);
  auto const results = other.host().take_results();
  REQUIRE(results.size() == 1);
  CHECK(results[0].id == id);
  CHECK(results[0].status == usb::transfer_status::completed);
  CHECK(results[0].data.size() == 18);
}

TEST_CASE("CDC bytes sent on the bulk OUT come back upper-cased on the bulk IN")
{
  rig const r;
  r.run_for(300 * ms);
  REQUIRE(r.host().state() == usb::device_state::configured);
  REQUIRE(r.t->controls == std::vector{usb::transfer_status::completed});
  r.t->to_send = {'h', 'e', 'l', 'l', 'o'};
  r.run_for(20 * ms);
  CHECK(std::string(r.t->received.begin(), r.t->received.end()) == "HELLO");
  CHECK(*r.logged == std::vector<std::string>{});
}

TEST_CASE("CDC bytes come back in order, across full and short packets and a ZLP")
{
  for(std::size_t const size : {64, 200, 1000})
  {
    CAPTURE(size);
    rig const r;
    r.run_for(300 * ms);
    REQUIRE(r.host().state() == usb::device_state::configured);
    std::vector<std::uint8_t> sent(size);
    for(std::size_t index = 0; index < sent.size(); ++index)
    {
      sent[index] = static_cast<std::uint8_t>('a' + index % 26);
    }
    r.t->to_send = sent;
    r.run_for(100 * ms);
    REQUIRE(r.t->received.size() == sent.size());
    std::vector<std::uint8_t> upper(sent.size());
    std::ranges::transform(sent, upper.begin(), [](std::uint8_t c) {
      return static_cast<std::uint8_t>(c - 'a' + 'A');
    });
    CHECK(r.t->received == upper);
    CHECK(*r.logged == std::vector<std::string>{});
  }
}

TEST_CASE("a pin the test drives makes the firmware send one HID report on the interrupt IN")
{
  rig const r;
  r.run_for(300 * ms);
  REQUIRE(r.host().state() == usb::device_state::configured);
  CHECK(r.t->reports.empty());
  r.press(true);
  r.run_for(50 * ms);
  REQUIRE(r.t->reports.size() == 1);
  CHECK(r.t->reports[0] == std::vector<std::uint8_t>{1, 1});
  r.press(false);
  r.run_for(20 * ms);
  r.press(true);
  r.run_for(50 * ms);
  REQUIRE(r.t->reports.size() == 2);
  CHECK(r.t->reports[1] == std::vector<std::uint8_t>{1, 2});
  CHECK(*r.logged == std::vector<std::string>{});
}
}

TEST_CASE("OTGRST detaches a configured device from the board host and keeps the cable as it is")
{
  {
    rig const r{false};
    r.run_for(300 * ms);
    REQUIRE(r.host().state() == usb::device_state::configured);
    rcc_reset_pulse(*r.f, "RCC_AHB2RSTR1", "OTGRST");
    CHECK(r.host().state() == usb::device_state::detached);
    CHECK(r.port().plugged());
  }
  rig const r{false};
  r.run_for(300 * ms);
  r.port().plug(false);
  rcc_reset_pulse(*r.f, "RCC_AHB2RSTR1", "OTGRST");
  CHECK_FALSE(r.port().plugged());
  CHECK(r.host().state() == usb::device_state::detached);
}
