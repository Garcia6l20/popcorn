#include "spi_medium.hpp"
#include "stm32_fixture.hpp"

#include <popcorn/arch/arch.hpp>
#include <popcorn/core/errors.hpp>
#include <popcorn/core/host.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/core/member_link.hpp>
#include <popcorn/core/snapshot.hpp>
#include <popcorn/core/system.hpp>
#include <popcorn/periph/stm32/board.hpp>
#include <popcorn/periph/stm32/uart_wire.hpp>

#include <reflex/serde/bson.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <random>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace
{
namespace core   = popcorn::core;
namespace periph = popcorn::periph;
namespace stm32  = popcorn::periph::stm32;
using popcorn::icount;
using popcorn::stop_reason;
using popcorn::vtime;
using popcorn::testing::stm32_fixture;
using popcorn::testing::sym;
using popcorn::testing::u575;

constexpr std::int64_t us_ps    = 1'000'000;
constexpr std::int64_t frame_ps = 4 * us_ps;
constexpr std::int64_t latency  = 20 * us_ps;

constexpr popcorn::address area      = 0x2008'0000u;
constexpr std::uint32_t    magic     = 0x5B1A11u;
constexpr std::uint32_t    master_fw = 1;
constexpr std::uint32_t    slave_fw  = 2;
constexpr std::uint32_t    boot      = 0xB000'0000u;

constexpr auto op(std::uint32_t code, std::uint32_t arg = 0) -> std::uint32_t
{
  return code << 24 | arg;
}

constexpr auto cs_low(std::uint32_t pin) -> std::uint32_t
{
  return op(1, pin);
}

constexpr auto cs_high(std::uint32_t pin) -> std::uint32_t
{
  return op(2, pin);
}

constexpr auto send(std::uint32_t count, std::uint32_t first) -> std::uint32_t
{
  return op(3, count << 8 | first);
}

constexpr auto wait_us(std::uint32_t us) -> std::uint32_t
{
  return op(4, us);
}

constexpr auto reset_op() -> std::uint32_t
{
  return op(5);
}

constexpr auto hard_nss() -> std::uint32_t
{
  return op(6, 1);
}

constexpr auto rcc_op() -> std::uint32_t
{
  return op(7);
}

constexpr auto stamp_op() -> std::uint32_t
{
  return op(8);
}

constexpr auto repeat(std::uint32_t count, std::uint32_t to) -> std::uint32_t
{
  return op(9, count << 8 | to);
}

constexpr auto kick(std::uint32_t word) -> std::uint32_t
{
  return op(10, word);
}

constexpr auto format_op(std::uint32_t bits, std::uint32_t prescaler) -> std::uint32_t
{
  return op(11, bits << 8 | prescaler);
}

void rcc_pulse(core::machine& target)
{
  auto const& desc  = target.peripheral("RCC").description();
  auto const* found = popcorn::find_register(desc, "RCC_APB2RSTR");
  REQUIRE(found != nullptr);
  auto const field =
      std::ranges::find(found->fields, std::string_view{"SPI1RST"}, &popcorn::field_desc::name);
  REQUIRE(field != found->fields.end());
  REQUIRE(target.mem().write(desc.base + found->offset, 4, 1u << field->lsb).has_value());
  REQUIRE(target.mem().write(desc.base + found->offset, 4, 0).has_value());
}

auto model_refused(core::machine& target) -> std::uint64_t
{
  return static_cast<std::uint64_t>(
      popcorn::testing::saved_integer(target, "SPI1", "refused").value_or(0));
}

class recording_link final : public core::member_link
{
public:
  explicit recording_link(std::shared_ptr<core::member_link> inner) : inner_{std::move(inner)}
  {}

  [[nodiscard]] auto sender() const noexcept -> std::size_t override
  {
    return inner_->sender();
  }

  [[nodiscard]] auto receiver() const noexcept -> std::size_t override
  {
    return inner_->receiver();
  }

  [[nodiscard]] auto latency() const noexcept -> core::sys_time override
  {
    return inner_->latency();
  }

  void enter_window(std::uint64_t window, core::time_map sender) override
  {
    inner_->enter_window(window, sender);
  }

  void sender_reset(core::sys_time at) override
  {
    sender_resets.push_back(at);
    inner_->sender_reset(at);
  }

  void receiver_reset(core::sys_time at) override
  {
    receiver_resets.push_back(at);
    inner_->receiver_reset(at);
  }

  auto drain(
      std::uint64_t         window,
      core::sys_time const& barrier,
      core::sys_time const& next,
      core::time_map        receiver) -> std::optional<core::link_fault> override
  {
    return inner_->drain(window, barrier, next, receiver);
  }

  [[nodiscard]] auto quiet() const -> bool override
  {
    return inner_->quiet();
  }

  void finish(core::sys_time at) override
  {
    inner_->finish(at);
  }

  void save(std::vector<std::byte>& out) const override
  {
    inner_->save(out);
  }

  void load(std::span<std::byte const> in) override
  {
    inner_->load(in);
  }

  [[nodiscard]] auto inner() const -> core::member_link&
  {
    return *inner_;
  }

  std::vector<core::sys_time> sender_resets;
  std::vector<core::sys_time> receiver_resets;

private:
  std::shared_ptr<core::member_link> inner_;
};

struct node
{
  stm32::board                   board{};
  std::unique_ptr<stm32_fixture> f;
  core::machine*                 target{};
  stm32::spi_link_port*          port{};
  std::int64_t                   origin{};

  node(std::uint32_t entry, std::vector<std::uint32_t> const& script, std::uint32_t prefill = 0)
  {
    auto options   = popcorn::testing::stm32_options();
    options.models = stm32::factories(board, u575());
    f              = std::make_unique<stm32_fixture>(u575(), "spi_ll", std::move(options));
    target         = f->target.get();
    REQUIRE(
        f->run_to_stop({.max_instructions = icount{100'000'000}, .until = sym("spi_ll", "main")})
            .reason
        == stop_reason::address);
    auto const put = [&](popcorn::address at, std::uint32_t value) {
      REQUIRE(target->mem().write(at, 4, value).has_value());
    };
    REQUIRE(script.size() < 64);
    put(area, magic);
    put(area + 4, entry);
    put(area + 8, 0);
    put(area + 12, 0);
    put(area + 16, 0);
    put(area + 20, prefill);
    for(std::size_t index = 0; index < 64; ++index)
    {
      put(area + 24 + 4 * static_cast<popcorn::address>(index),
          index < script.size() ? script[index] : 0);
    }
    f->set("pc", sym("spi_ll", "link_run"));
    origin = static_cast<std::int64_t>(target->virtual_clock().time().ps);
    port   = f->model_of<stm32::spi_link_port>("SPI1");
    REQUIRE(port != nullptr);
  }

  [[nodiscard]] auto log() const -> std::vector<std::uint32_t>
  {
    auto const                 count = word(area + 12);
    std::vector<std::uint32_t> out;
    for(std::uint32_t index = 0; index < count and index < 256; ++index)
    {
      out.push_back(word(area + 280 + 4 * index));
    }
    return out;
  }

  [[nodiscard]] auto word(popcorn::address at) const -> std::uint32_t
  {
    auto const value = target->mem().read(at, 4);
    REQUIRE(value.has_value());
    return static_cast<std::uint32_t>(*value);
  }
};

class quiet_link final : public core::member_link
{
public:
  quiet_link(std::size_t from, std::size_t to, std::int64_t ps) : from_{from}, to_{to}, ps_{ps}
  {}

  [[nodiscard]] auto sender() const noexcept -> std::size_t override
  {
    return from_;
  }

  [[nodiscard]] auto receiver() const noexcept -> std::size_t override
  {
    return to_;
  }

  [[nodiscard]] auto latency() const noexcept -> core::sys_time override
  {
    return {ps_};
  }

  void enter_window(std::uint64_t, core::time_map) override
  {}

  void sender_reset(core::sys_time) override
  {}

  void receiver_reset(core::sys_time) override
  {}

  auto drain(std::uint64_t, core::sys_time const&, core::sys_time const&, core::time_map)
      -> std::optional<core::link_fault> override
  {
    return std::nullopt;
  }

  [[nodiscard]] auto quiet() const -> bool override
  {
    return true;
  }

  void finish(core::sys_time) override
  {}

  void save(std::vector<std::byte>&) const override
  {}

  void load(std::span<std::byte const>) override
  {}

private:
  std::size_t  from_;
  std::size_t  to_;
  std::int64_t ps_;
};

struct variant
{
  std::string_view             name;
  std::int64_t                 lowered{};
  core::slice_bounds           bounds{};
  std::optional<std::uint32_t> seed{};
};

auto variants() -> std::vector<variant>
{
  core::slice_bounds const fine{
      .max_instructions = icount{997}, .wall_budget = std::chrono::microseconds{10}};
  return {
      {.name = "q=l"},
      {.name = "q=l/2", .lowered = 2},
      {.name = "997 q=l/3", .lowered = 3, .bounds = fine},
      {.name = "sleeps q=l/2", .lowered = 2, .seed = 1},
  };
}

struct slave_spec
{
  std::vector<std::uint32_t>  script;
  std::optional<std::uint8_t> cs_pin;
  std::uint32_t               prefill{};
};

struct world
{
  std::vector<std::unique_ptr<node>>              nodes;
  std::shared_ptr<stm32::spi_medium>              medium;
  std::vector<std::shared_ptr<core::member_link>> links;
  std::vector<core::member_ref>                   members;
  std::unique_ptr<core::system>                   sys;
  std::vector<stm32::spi_tap_frame>               taps;
  std::vector<std::shared_ptr<recording_link>>    recorded;
  std::int64_t                                    l{latency};
  std::int64_t                                    q{latency};

  world(
      std::vector<std::uint32_t> const& master,
      std::vector<slave_spec> const&    slaves,
      std::int64_t                      link_latency = latency)
      : l{link_latency}
  {
    nodes.push_back(std::make_unique<node>(master_fw, master));
    std::vector<stm32::spi_slave_end> ends;
    for(auto const& [index, slave] : std::views::enumerate(slaves))
    {
      nodes.push_back(std::make_unique<node>(slave_fw, slave.script, slave.prefill));
      ends.push_back(
          {.member     = static_cast<std::size_t>(index) + 1,
           .port       = nodes.back()->port,
           .cs_port    = "GPIOA",
           .cs_pin     = slave.cs_pin,
           .forth_lane = 1,
           .back_lane  = static_cast<std::uint32_t>(index) + 1});
    }
    medium = std::make_shared<stm32::spi_medium>(0, *nodes[0]->port, core::sys_time{l}, ends);
    medium->attach_master();
    for(std::size_t index = 0; index < slaves.size(); ++index)
    {
      medium->attach_slave(index);
    }
    for(auto const& link : medium->links())
    {
      recorded.push_back(std::make_shared<recording_link>(link));
      links.push_back(recorded.back());
    }
  }

  void host(variant const& how)
  {
    q = l;
    if(how.lowered != 0)
    {
      q = l / how.lowered;
      links.push_back(std::make_shared<quiet_link>(0, 1, q));
    }
    for(auto& one : nodes)
    {
      std::vector<std::unique_ptr<core::machine>> machines;
      machines.push_back(std::move(one->f->target));
      members.push_back(
          {.on   = std::make_shared<core::host>(std::move(machines)),
           .name = std::format("m{}", members.size())});
    }
    std::move_only_function<void(std::size_t)> hook;
    if(how.seed)
    {
      auto engines = std::make_shared<std::vector<std::mt19937>>();
      for(std::size_t index = 0; index < nodes.size(); ++index)
      {
        engines->emplace_back(*how.seed * 31 + index);
      }
      hook = [engines](std::size_t index) {
        std::uniform_int_distribution<int> pause{0, 50};
        std::this_thread::sleep_for(std::chrono::microseconds{pause((*engines)[index])});
      };
    }
    sys = std::make_unique<core::system>(members, links, how.bounds, std::move(hook), nullptr);
    std::vector<core::sys_time> offsets(nodes.size());
    auto const                  held = sys->lock();
    sys->start(held, offsets);
  }

  auto resume(std::int64_t until) -> core::pause
  {
    auto const pending = [&] {
      auto const held = sys->lock();
      return sys->resume(held, core::sys_time{until});
    }();
    return pending.get();
  }

  auto run_to(std::int64_t until) -> core::pause
  {
    for(;;)
    {
      auto const stopped = resume(until);
      if(stopped.reason != core::pause_reason::delivered)
      {
        take_taps();
        return stopped;
      }
      auto const held = sys->lock();
      sys->release(held, *stopped.member, true);
    }
  }

  void take_taps()
  {
    if(medium)
    {
      auto taken = medium->tap().take();
      taps.insert(taps.end(), taken.begin(), taken.end());
    }
  }

  auto idle() -> core::pause
  {
    std::vector<core::host_future<core::goal_reached>> goals;
    auto const                                         pending = [&] {
      auto const held = sys->lock();
      for(std::size_t member = 0; member < members.size(); ++member)
      {
        goals.push_back(sys->post_goal(held, member, {.until = popcorn::address{0x0800'0001}}));
      }
      return sys->resume(held);
    }();
    auto const stopped = pending.get();
    take_taps();
    return stopped;
  }

  template <typename F> auto on(std::size_t member, F&& fn)
  {
    return core::access(*members[member].on, 0, std::forward<F>(fn));
  }

  void reset(std::size_t member, popcorn::reset_kind kind = popcorn::reset_kind::software)
  {
    auto const held = sys->lock();
    sys->reset(held, member, kind);
  }

  void rcc(std::size_t member)
  {
    on(member, [](core::machine& target) { rcc_pulse(target); });
  }

  [[nodiscard]] auto refused() -> std::uint64_t
  {
    auto total = medium->refused();
    for(std::size_t member = 0; member < nodes.size(); ++member)
    {
      total += on(member, [](core::machine& target) { return model_refused(target); });
    }
    return total;
  }

  [[nodiscard]] auto frames_of(std::size_t slave) const -> std::vector<stm32::spi_tap_frame>
  {
    std::vector<stm32::spi_tap_frame> out;
    for(auto const& frame : sorted_taps())
    {
      if(frame.slave == slave)
      {
        out.push_back(frame);
      }
    }
    return out;
  }

  [[nodiscard]] auto log(std::size_t member) -> std::vector<std::uint32_t>
  {
    return on(member, [&](core::machine&) { return nodes[member]->log(); });
  }

  [[nodiscard]] auto sorted_taps() const -> std::vector<stm32::spi_tap_frame>
  {
    auto out = taps;
    std::ranges::sort(out, {}, [](stm32::spi_tap_frame const& frame) {
      return std::pair{frame.slave, frame.unit};
    });
    return out;
  }

  [[nodiscard]] auto dump() -> std::string
  {
    std::string out;
    for(std::size_t member = 0; member < nodes.size(); ++member)
    {
      out += std::format("m{}:", member);
      for(auto const word : log(member))
      {
        out += std::format(" {:x}", word);
      }
      out += "\n";
    }
    for(auto const& frame : sorted_taps())
    {
      out += std::format(
          "s{} u{} at {} reply {} {:x}->{:x} {}\n", frame.slave, frame.unit, frame.at.ps,
          frame.reply.ps, frame.mosi, frame.miso, frame.dropped ? "dropped" : "");
    }
    out += std::format("refused {}\n", refused());
    for(auto const& link : recorded)
    {
      for(auto const& at : link->sender_resets)
      {
        out += std::format("sender reset {}\n", at.ps);
      }
      for(auto const& at : link->receiver_resets)
      {
        out += std::format("receiver reset {}\n", at.ps);
      }
    }
    for(std::size_t index = 0; index < medium->slave_count(); ++index)
    {
      out += std::format("owed {} tickets {}\n", medium->owed(index), medium->tickets(index));
    }
    return out;
  }
};

void keep_dump(std::string_view name, std::string const& bytes)
{
  if(auto const* dir = std::getenv("POPCORN_SYSTEM_DUMP"); dir != nullptr)
  {
    std::string file{name};
    std::ranges::replace(file, ' ', '_');
    std::ranges::replace(file, '/', '_');
    std::ofstream{std::string{dir} + "/" + file + ".txt", std::ios::binary} << bytes;
  }
}

auto echo_slave(std::optional<std::uint8_t> cs_pin) -> slave_spec
{
  return {.script = {}, .cs_pin = cs_pin};
}

using build_fn  = std::copyable_function<world() const>;
using build_ref = std::function_ref<world() const>;
using check_fn  = std::function_ref<void(world&)>;

void no_check(world&)
{}

struct step
{
  std::int64_t                               at{};
  std::copyable_function<void(world&) const> act;
};

auto same_everywhere(
    std::string_view         name,
    build_ref                build,
    std::int64_t             end,
    check_fn                 check = no_check,
    std::vector<step> const& steps = {},
    std::vector<variant>     ways  = variants()) -> std::string
{
  std::optional<std::string> reference;
  for(auto const& how : ways)
  {
    CAPTURE(how.name);
    auto w = build();
    w.host(how);
    for(auto const& one : steps)
    {
      REQUIRE(w.run_to(one.at).reason == core::pause_reason::system_time);
      one.act(w);
    }
    REQUIRE(w.run_to(end).reason == core::pause_reason::system_time);
    auto const seen = w.dump();
    keep_dump(std::format("{} {}", name, how.name), seen);
    check(w);
    if(reference)
    {
      CHECK(seen == *reference);
    }
    else
    {
      reference = seen;
    }
  }
  return *reference;
}
}

TEST_CASE("two slaves on two GPIO chip selects each get their own frames at both ends")
{
  auto const build = [] {
    return world{
        {cs_high(4), cs_high(5), wait_us(50), cs_low(4), send(3, 0x10), cs_high(4), cs_low(5),
         send(3, 0x20), cs_high(5), send(1, 0x30)},
        {echo_slave(4), echo_slave(5)}
    };
  };
  auto const seen = same_everywhere("two slaves", build, 2'000 * us_ps, [](world& w) {
    auto const master = w.log(0);
    CHECK(master == std::vector<std::uint32_t>{boot, 0xA0, 0x20, 0x21, 0xA0, 0x30, 0x31, 0xFF});
    CHECK(w.log(1) == std::vector<std::uint32_t>{boot, 0x5110, 0x5111, 0x5112});
    CHECK(w.log(2) == std::vector<std::uint32_t>{boot, 0x5120, 0x5121, 0x5122});
    auto const taps = w.sorted_taps();
    REQUIRE(taps.size() == 6);
    for(auto const& frame : taps)
    {
      CHECK(frame.reply.ps == frame.at.ps + w.l + frame_ps);
      CHECK_FALSE(frame.dropped);
    }
    CHECK(w.medium->refused() == 0);
    CHECK(w.medium->owed(0) == 0);
    CHECK(w.medium->tickets(0) == 0);
  });
}

namespace
{
constexpr std::int64_t row_end = 1'500 * us_ps;

auto ceil_after(std::int64_t at, std::int64_t q) -> std::int64_t
{
  return (at / q + 1) * q;
}

auto handed_by(std::int64_t stamp, std::int64_t at, std::int64_t l, std::int64_t q) -> bool
{
  return ceil_after(stamp + l - q, q) < at;
}

auto drained_by(std::int64_t stamp, std::int64_t at, std::int64_t q) -> bool
{
  return ceil_after(stamp, q) < at;
}

auto try_pick(std::int64_t from, std::int64_t to, std::function_ref<bool(std::int64_t)> fits)
    -> std::optional<std::int64_t>
{
  for(auto at = from + us_ps / 8; at < to; at += us_ps / 8)
  {
    if(fits(at))
    {
      return at;
    }
  }
  return std::nullopt;
}

auto pick(std::int64_t from, std::int64_t to, std::function_ref<bool(std::int64_t)> fits)
    -> std::int64_t
{
  auto const found = try_pick(from, to, fits);
  REQUIRE(found.has_value());
  return *found;
}

auto master_script(std::uint32_t frames = 3, std::uint32_t wait = 50) -> std::vector<std::uint32_t>
{
  return {cs_high(4), wait_us(wait), cs_low(4), send(frames, 0x10), cs_high(4)};
}

auto single(std::vector<std::uint32_t> master = master_script(), std::int64_t l = latency)
    -> build_fn
{
  return [master, l] { return world{master, {echo_slave(4)}, l}; };
}

auto reference(build_ref build, std::int64_t end = row_end) -> std::vector<stm32::spi_tap_frame>
{
  auto w = build();
  w.host({.name = "reference"});
  REQUIRE(w.run_to(end).reason == core::pause_reason::system_time);
  return w.frames_of(0);
}

void one_reply_each(world& w, std::size_t frames)
{
  auto const seen = w.frames_of(0);
  REQUIRE(seen.size() == frames);
  for(std::size_t index = 1; index < seen.size(); ++index)
  {
    CHECK(seen[index].unit > seen[index - 1].unit);
  }
  CHECK(w.medium->owed(0) == 0);
  CHECK(w.medium->tickets(0) == 0);
}
}

TEST_CASE("R1: a slave RCC reset under a frame answers it with all ones at the frame end")
{
  auto const build = single();
  auto const t     = reference(build)[0].at.ps;
  same_everywhere(
      "R1", build, row_end,
      [&](world& w) {
        one_reply_each(w, 3);
        auto const seen = w.frames_of(0);
        CHECK(seen[0].dropped);
        CHECK(seen[0].reply.ps == t + w.l + frame_ps);
        CHECK(seen[0].miso == 0xFF);
        CHECK_FALSE(seen[1].dropped);
        CHECK(seen[1].miso == 0xFF);
        CHECK(w.refused() == 0);
  },
      {{.at = t + latency + frame_ps / 2, .act = [](world& w) { w.rcc(1); }}});
}

TEST_CASE(
    "R1b: a frame handed before a slave RCC reset and starting after it runs in the fresh SPI")
{
  auto const build = single(master_script(3, 61));
  auto const t     = reference(build)[0].at.ps;
  auto const at    = pick(t, t + latency, [&](std::int64_t x) {
    return handed_by(t, x, latency, latency) and not handed_by(t, x, latency, latency / 2);
  });
  same_everywhere(
      "R1b", build, row_end,
      [&](world& w) {
        one_reply_each(w, 3);
        for(auto const& frame : w.frames_of(0))
        {
          CHECK_FALSE(frame.dropped);
          CHECK(frame.reply.ps == frame.at.ps + w.l + frame_ps);
        }
  },
      {{.at = at, .act = [](world& w) { w.rcc(1); }}});
}

TEST_CASE("R2: a slave member reset under a running frame answers it at the frame end")
{
  auto const build = single();
  auto const t     = reference(build)[0].at.ps;
  same_everywhere(
      "R2", build, row_end,
      [&](world& w) {
        one_reply_each(w, 3);
        auto const seen = w.frames_of(0);
        CHECK(seen[0].dropped);
        CHECK(seen[0].reply.ps == t + w.l + frame_ps);
        REQUIRE(w.recorded[0]->receiver_resets.size() == 1);
        CHECK(w.recorded[0]->receiver_resets[0].ps == t + latency + frame_ps / 2);
  },
      {{.at = t + latency + frame_ps / 2, .act = [](world& w) { w.reset(1); }}});
}

TEST_CASE("R3: a slave member reset before a queued frame starts answers it at the frame end")
{
  auto const build = single();
  auto const t     = reference(build)[0].at.ps;
  auto const at    = pick(t, t + latency, [&](std::int64_t x) {
    return std::ranges::all_of(std::array{latency, latency / 2, latency / 3}, [&](auto q) {
      return handed_by(t, x, latency, q);
    });
  });
  same_everywhere(
      "R3", build, row_end,
      [&](world& w) {
        one_reply_each(w, 3);
        auto const seen = w.frames_of(0);
        CHECK(seen[0].dropped);
        CHECK(seen[0].reply.ps == t + w.l + frame_ps);
  },
      {{.at = at, .act = [](world& w) { w.reset(1); }}});
}

TEST_CASE("R4 and R5: a frame still in the link at a slave reset is answered at its far-side end")
{
  auto const build = single();
  auto const t     = reference(build)[0].at.ps;
  SUBCASE("R4, in the mailbox at every Q")
  {
    auto const at = pick(t, t + us_ps, [&](std::int64_t x) {
      return std::ranges::none_of(std::array{latency, latency / 2, latency / 3}, [&](auto q) {
        return drained_by(t, x, q);
      });
    });
    same_everywhere(
        "R4", build, row_end,
        [&](world& w) {
          one_reply_each(w, 3);
          CHECK(w.frames_of(0)[0].dropped);
          CHECK(w.frames_of(0)[0].reply.ps == t + w.l + frame_ps);
    },
        {{.at = at, .act = [](world& w) { w.reset(1); }}});
  }
  SUBCASE("R5, carried past a drain at L above Q")
  {
    auto const at = pick(t, t + latency, [&](std::int64_t x) {
      return drained_by(t, x, latency / 2) and not handed_by(t, x, latency, latency / 2);
    });
    same_everywhere(
        "R5", build, row_end,
        [&](world& w) {
          one_reply_each(w, 3);
          CHECK(w.frames_of(0)[0].dropped);
          CHECK(w.frames_of(0)[0].reply.ps == t + w.l + frame_ps);
    },
        {{.at = at, .act = [](world& w) { w.reset(1); }}});
  }
}

TEST_CASE("R6: a frame stamped after a slave reset reaches the fresh slave")
{
  auto const build = single();
  auto const t     = reference(build)[0].at.ps;
  same_everywhere(
      "R6", build, row_end,
      [&](world& w) {
        one_reply_each(w, 3);
        for(auto const& frame : w.frames_of(0))
        {
          CHECK_FALSE(frame.dropped);
          CHECK(frame.reply.ps == frame.at.ps + w.l + frame_ps);
        }
  },
      {{.at = t - 2 * us_ps, .act = [](world& w) { w.reset(1); }}});
}

TEST_CASE(
    "R7: a select stamped before a slave reset reaches the fresh slave from the link or the port")
{
  constexpr std::int64_t long_l = 1'000 * us_ps;
  constexpr std::int64_t end    = 6'000 * us_ps;
  auto const             build  = single(
      {cs_high(4), wait_us(650), stamp_op(), cs_low(4), wait_us(450), send(2, 0x10), cs_high(4)},
      long_l);
  auto const select = [&] {
    auto w = build();
    w.host({.name = "reference"});
    REQUIRE(w.run_to(end).reason == core::pause_reason::system_time);
    auto const log = w.log(0);
    REQUIRE(log.size() >= 2);
    REQUIRE((log[1] >> 24) == 0x5Au);
    return static_cast<std::int64_t>(log[1] & 0xffffffu) * 250'000;
  }();
  auto const at = pick(select, select + long_l - 600 * us_ps, [&](std::int64_t x) {
    return handed_by(select + 3 * us_ps, x, long_l, long_l)
       and not handed_by(select, x, long_l, long_l / 2);
  });
  same_everywhere(
      "R7", build, end,
      [&](world& w) {
        auto const seen = w.frames_of(0);
        REQUIRE(seen.size() == 2);
        CHECK_FALSE(seen[0].dropped);
        CHECK(seen[0].at.ps > at);
        CHECK(seen[0].miso == 0xA0);
        CHECK(w.log(1) == std::vector<std::uint32_t>{boot, boot, 0x5110, 0x5111}
          );
  },
      {{.at = at, .act = [](world& w) { w.reset(1); }}},
      {{.name = "q=l"}, {.name = "q=l/2", .lowered = 2}, {.name = "q=l/3", .lowered = 3}});
}

TEST_CASE("R8: a master member reset mid-frame deselects, refuses the late reply and reselects")
{
  auto const build =
      single({cs_high(4), wait_us(50), cs_low(4), send(1, 0x10), send(1, 0x20), cs_high(4)});
  auto const t = reference(build)[0].at.ps;
  SUBCASE("the deselect cuts the slave frame, its reply stamped after t_m is refused")
  {
    auto const at = t + frame_ps / 2;
    same_everywhere(
        "R8 cut", build, row_end,
        [&](world& w) {
          auto const seen = w.frames_of(0);
          REQUIRE(seen.size() == 2);
          CHECK(seen[0].dropped);
          CHECK(seen[0].reply.ps == at + w.l);
          CHECK_FALSE(seen[1].dropped);
          CHECK(w.refused() == 1);
          REQUIRE(w.recorded[0]->sender_resets.size() == 1);
          CHECK(w.recorded[0]->sender_resets[0].ps == at);
          CHECK(std::ranges::contains(w.log(1), 0x5120u));
          CHECK(w.medium->owed(0) == 0);
          CHECK(w.medium->tickets(0) == 0);
    },
        {{.at = at, .act = [](world& w) { w.reset(0); }}});
  }
  SUBCASE("an SS-bound slave hears the reset's deselect once, stamped at t_m")
  {
    build_fn const ss = [] {
      return world{
          {hard_nss(), wait_us(50), send(1, 0x10), send(1, 0x20)},
          {echo_slave(std::nullopt)}
      };
    };
    auto const ts = reference(ss)[0].at.ps;
    auto const at = ts + frame_ps / 2;
    same_everywhere(
        "R8 ss", ss, row_end,
        [&](world& w) {
          auto const seen = w.frames_of(0);
          REQUIRE(seen.size() == 2);
          CHECK(seen[0].dropped);
          CHECK(seen[0].reply.ps == at + w.l);
          CHECK(w.refused() == 1);
          CHECK(std::ranges::contains(w.log(1), 0x5120u));
    },
        {{.at = at, .act = [](world& w) { w.reset(0); }}});
  }
  SUBCASE("a reply stamped before t_m is dropped or wiped and not counted")
  {
    auto const at = t + latency + frame_ps + latency / 2 + us_ps / 4;
    same_everywhere(
        "R8 late", build, row_end,
        [&](world& w) {
          auto const seen = w.frames_of(0);
          REQUIRE(seen.size() == 2);
          CHECK_FALSE(seen[0].dropped);
          CHECK(w.refused() == 0);
          CHECK(std::ranges::contains(w.log(1), 0x5120u));
    },
        {{.at = at, .act = [](world& w) { w.reset(0); }}});
  }
}

TEST_CASE("R8: a master firmware reset mid-window with chip select low reaches the slave again")
{
  auto const build = single(
      {cs_high(4), wait_us(50), cs_low(4), kick(0x10), wait_us(6), reset_op(), send(1, 0x20),
       cs_high(4)});
  same_everywhere("R8 firmware", build, row_end, [&](world& w) {
    auto const seen = w.frames_of(0);
    REQUIRE(seen.size() == 2);
    CHECK(w.refused() == 1);
    REQUIRE(w.recorded[0]->sender_resets.size() == 1);
    CHECK(std::ranges::contains(w.log(1), 0x5120u));
    CHECK(w.medium->owed(0) == 0);
  });
}

TEST_CASE("R9: a master RCC reset refuses the kept reply once at every Q")
{
  for(std::optional<std::uint8_t> const cs :
      std::array<std::optional<std::uint8_t>, 2>{std::optional<std::uint8_t>{4}, std::nullopt})
  {
    CAPTURE(cs.has_value());
    build_fn                    build;
    std::optional<std::int64_t> at;
    for(std::uint32_t wait = 50; wait < 70 and not at; ++wait)
    {
      auto const script =
          cs ? std::vector<std::uint32_t>{cs_high(4), wait_us(wait), cs_low(4), send(1, 0x10)}
             : std::vector<std::uint32_t>{hard_nss(), wait_us(wait), send(1, 0x10)};
      build            = [script, cs] { return world{script, {echo_slave(cs)}}; };
      auto const reply = reference(build)[0].reply.ps;
      at               = try_pick(reply, reply + latency, [&](std::int64_t x) {
        return handed_by(reply, x, latency, latency)
           and not handed_by(reply, x, latency, latency / 2);
      });
    }
    REQUIRE(at.has_value());
    same_everywhere(
        cs ? "R9 gpio" : "R9 ss", build, row_end,
        [&](world& w) {
          auto const seen = w.frames_of(0);
          REQUIRE(seen.size() == 1);
          CHECK_FALSE(seen[0].dropped);
          CHECK(w.refused() == 1);
          CHECK(w.recorded[0]->sender_resets.empty());
    },
        {{.at = *at, .act = [](world& w) { w.rcc(0); }}});
  }
}

TEST_CASE("R10 and R11: master and slave member resets in one window give one reply per unit")
{
  auto const build = single();
  auto const t     = reference(build)[0].at.ps;
  for(bool const master_first : {true, false})
  {
    CAPTURE(master_first);
    auto const first  = t + frame_ps / 2;
    auto const second = first + 2 * us_ps;
    same_everywhere(
        master_first ? "R10" : "R11", build, row_end,
        [&](world& w) {
          auto const seen = w.frames_of(0);
          REQUIRE(seen.size() == 1);
          CHECK(seen[0].dropped);
          CHECK(seen[0].miso == 0xFF);
          CHECK(seen[0].reply.ps == t + w.l + frame_ps);
          REQUIRE(w.recorded[0]->sender_resets.size() == 1);
          REQUIRE(w.recorded[0]->receiver_resets.size() == 1);
          CHECK(
              (w.recorded[0]->sender_resets[0] < w.recorded[0]->receiver_resets[0])
              == master_first);
          CHECK(w.refused() == 1);
          CHECK(w.medium->owed(0) == 0);
          CHECK(w.medium->tickets(0) == 0);
    },
        {{.at = first, .act = [master_first](world& w) { w.reset(master_first ? 0 : 1); }},
         {.at = second, .act = [master_first](world& w) { w.reset(master_first ? 1 : 0); }}});
  }
}

TEST_CASE("R10b: a master deselect handed before a later slave reset keeps the dropped answer")
{
  auto const base = latency / 3;
  auto const half = latency / 6;
  auto const gap  = [&](std::int64_t x) {
    return x + latency - half - ceil_after(x + latency - base, base);
  };
  build_fn     build;
  std::int64_t t  = 0;
  std::int64_t tm = 0;
  for(std::uint32_t wait = 50; wait < 60 and gap(tm) <= us_ps; ++wait)
  {
    build = single(master_script(3, wait));
    t     = reference(build)[0].at.ps;
    tm    = t + us_ps / 8;
    for(auto x = tm; x < t + frame_ps - 2 * us_ps; x += us_ps / 8)
    {
      tm = gap(x) > gap(tm) ? x : tm;
    }
  }
  REQUIRE(gap(tm) > us_ps);
  auto const ts = ceil_after(tm + latency - base, base) + gap(tm) / 2;
  same_everywhere(
      "R10b", build, row_end,
      [&](world& w) {
        auto const seen = w.frames_of(0);
        REQUIRE_FALSE(seen.empty());
        CHECK(seen[0].dropped);
        REQUIRE(w.recorded[0]->receiver_resets.size() == 1);
        REQUIRE(w.recorded[0]->sender_resets.size() == 1);
        CHECK(w.recorded[0]->sender_resets[0].ps + w.l < t + w.l + frame_ps);
        CHECK(
            seen[0].reply.ps == std::max(w.recorded[0]->receiver_resets[0].ps, t + w.l + frame_ps));
        CHECK(w.medium->owed(0) == 0);
  },
      {{.at = tm, .act = [](world& w) { w.reset(0); }},
       {.at = ts, .act = [](world& w) { w.reset(1); }}},
      {{.name = "q=l/3", .lowered = 3}, {.name = "q=l/6", .lowered = 6}});
}

TEST_CASE("R12: two slave resets before the re-armed entry fires give one reply")
{
  auto const build = single();
  auto const t     = reference(build)[0].at.ps;
  same_everywhere(
      "R12", build, row_end,
      [&](world& w) {
        one_reply_each(w, 3);
        CHECK(w.frames_of(0)[0].dropped);
        REQUIRE(w.recorded[0]->receiver_resets.size() == 2);
        CHECK(
            w.frames_of(0)[0].reply.ps
            == std::max(w.recorded[0]->receiver_resets[1].ps, t + w.l + frame_ps));
  },
      {{.at = t + latency + us_ps, .act = [](world& w) { w.reset(1); }},
       {.at = t + latency + 3 * us_ps, .act = [](world& w) { w.reset(1); }}});
}

TEST_CASE("two slaves on the SS output are both selected and their replies are ANDed")
{
  auto const build = [] {
    return world{
        {hard_nss(), wait_us(50), send(3, 0x10)},
        {slave_spec{.script = {}, .cs_pin = std::nullopt},
         slave_spec{.script = {}, .cs_pin = std::nullopt, .prefill = 0x3C}}
    };
  };
  same_everywhere("nss", build, row_end, [](world& w) {
    CHECK(w.log(0) == std::vector<std::uint32_t>{boot, 0x20, 0x20, 0x21});
    CHECK(w.log(1) == std::vector<std::uint32_t>{boot, 0x5110, 0x5111, 0x5112});
    CHECK(w.log(2) == std::vector<std::uint32_t>{boot, 0x5110, 0x5111, 0x5112});
    CHECK(w.frames_of(0).size() == 3);
    CHECK(w.frames_of(1).size() == 3);
    for(auto const& frame : w.sorted_taps())
    {
      CHECK(frame.reply.ps == frame.at.ps + w.l + frame_ps);
      CHECK_FALSE(frame.cs_pin.has_value());
    }
  });
}

TEST_CASE("1000 master resets mid-frame keep at most two owed units and one ticket")
{
  auto const build = single({kick(0x10), wait_us(10), reset_op(), repeat(1000, 0)});
  same_everywhere(
      "owed bound", build, 600'000 * us_ps,
      [](world& w) {
        CHECK(w.frames_of(0).size() == 1000);
        CHECK(w.medium->owed_peak(0) <= 2);
        CHECK(w.medium->owed(0) == 0);
        CHECK(w.medium->tickets_peak(0) == 1);
        CHECK(w.medium->tickets(0) == 0);
        CHECK(w.refused() == 1000);
        REQUIRE(w.recorded[0]->sender_resets.size() == 1000);
  },
      {}, {{.name = "q=l"}, {.name = "q=l/2", .lowered = 2}});
}

TEST_CASE("four master resets in one window keep at most five owed units")
{
  constexpr std::int64_t long_l = 3'000 * us_ps;
  auto const             build =
      single({kick(0x10), wait_us(6), reset_op(), repeat(4, 0), send(1, 0x20)}, long_l);
  same_everywhere(
      "owed packed", build, 12'000 * us_ps,
      [](world& w) {
        CHECK(w.frames_of(0).size() == 5);
        CHECK(w.medium->owed_peak(0) == (w.q == w.l ? 5u : 3u));
        CHECK(w.medium->owed(0) == 0);
        CHECK(w.medium->tickets(0) == 0);
  },
      {}, {{.name = "q=l"}, {.name = "q=l/2", .lowered = 2}});
}

TEST_CASE("two master RCC resets inside a round trip keep three tickets, none at rest")
{
  constexpr std::int64_t long_l = 60 * us_ps;
  auto const             build  = single(
      {wait_us(50), kick(0x10), wait_us(6), rcc_op(), kick(0x11), wait_us(6), rcc_op(),
       send(1, 0x12)},
      long_l);
  same_everywhere("tickets rcc", build, row_end, [](world& w) {
    CHECK(w.frames_of(0).size() == 3);
    CHECK(w.medium->tickets_peak(0) == (w.q == w.l ? 2u : 3u));
    CHECK(w.medium->tickets(0) == 0);
    CHECK(w.refused() == 2);
  });
}

TEST_CASE("frames after a software reset and a power-on reset of the master keep both ends' stamps")
{
  auto const build = single(
      {cs_high(4), wait_us(50), cs_low(4), send(1, 0x10), wait_us(800), stamp_op(), send(1, 0x11),
       wait_us(800), send(1, 0x12)});
  auto const t = reference(build)[0].at.ps;
  same_everywhere(
      "time bases", build, 3'500 * us_ps,
      [&](world& w) {
        auto const seen = w.frames_of(0);
        REQUIRE(seen.size() == 3);
        REQUIRE(w.recorded[0]->sender_resets.size() == 2);
        auto const software = w.recorded[0]->sender_resets[0].ps;
        auto const power_on = w.recorded[0]->sender_resets[1].ps;
        auto const log      = w.log(0);
        auto const stamp =
            std::ranges::find_if(log, [](std::uint32_t word) { return word >> 24 == 0x5Au; }
               );
        REQUIRE(stamp != log.end());
        auto const stamped = software + w.nodes[0]->origin + (*stamp & 0xffffffu) * 250'000;
        CHECK(seen[1].mosi == 0x11);
        CHECK(seen[1].at.ps >= stamped);
        CHECK(seen[1].at.ps <= stamped + 40 * us_ps);
        CHECK(seen[1].at.ps < power_on);
        CHECK(seen[2].mosi == 0x12);
        CHECK(seen[2].at.ps > power_on);
        for(auto const& frame : seen)
        {
          CHECK(frame.reply.ps == frame.at.ps + w.l + frame_ps);
        }
        CHECK(std::ranges::contains(w.log(1), 0x5111u));
        CHECK(std::ranges::contains(w.log(1), 0x5112u));
  },
      {{.at = t + 100 * us_ps, .act = [](world& w) { w.reset(0); }},
       {.at  = t + 1'100 * us_ps,
        .act = [](world& w) { w.reset(0, popcorn::reset_kind::power_on); }}});
}

TEST_CASE("give_back completes the frame the master waits for, the transfer ends")
{
  auto const build = single({cs_high(4), wait_us(50), cs_low(4), send(1, 0x10), send(1, 0x11)});
  auto const t     = reference(build)[0].at.ps;
  same_everywhere(
      "give back", build, row_end,
      [](world& w) {
        CHECK(w.log(0) == std::vector<std::uint32_t>{boot, 0xFF, 0xFF}
          );
        CHECK(w.medium->tickets(0) == 0);
  },
      {{.at  = t + latency,
        .act = [](world& w) { w.on(0, [&](core::machine&) { w.medium->give_back_master(); }); }}});
}

namespace
{
struct captured
{
  std::vector<core::snapshot> members;
  std::vector<std::byte>      system;
};

auto capture(world& w) -> captured
{
  captured out;
  for(std::size_t member = 0; member < w.nodes.size(); ++member)
  {
    out.members.push_back(w.on(member, [](core::machine& target) { return target.snapshot(); }));
  }
  auto const held = w.sys->lock();
  w.sys->save(held, out.system);
  return out;
}

void put_back(world& w, captured const& saved)
{
  for(std::size_t member = 0; member < w.nodes.size(); ++member)
  {
    w.on(member, [&](core::machine& target) { target.restore(saved.members[member]); });
  }
  for(std::size_t member = 0; member < w.nodes.size(); ++member)
  {
    w.on(member, [&](core::machine& target) {
      auto        again = target.snapshot().data();
      auto const& was   = saved.members[member].data();
      CHECK(again.generation > was.generation);
      again.generation = was.generation;
      CHECK(core::encode_snapshot(again) == core::encode_snapshot(was));
    });
  }
  auto const held = w.sys->lock();
  w.sys->load(held, saved.system);
  std::vector<std::byte> again;
  w.sys->save(held, again);
  CHECK(again == saved.system);
}

auto restored_run(
    build_ref                       build,
    std::int64_t                    at,
    std::function_ref<void(world&)> act,
    std::int64_t                    end = row_end)
{
  for(auto const& how : variants())
  {
    CAPTURE(how.name);
    auto w = build();
    w.host(how);
    REQUIRE(w.run_to(at).reason == core::pause_reason::system_time);
    act(w);
    auto const saved = capture(w);
    w.taps.clear();
    REQUIRE(w.run_to(end).reason == core::pause_reason::system_time);
    auto const first = w.dump();
    put_back(w, saved);
    w.taps.clear();
    REQUIRE(w.run_to(end).reason == core::pause_reason::system_time);
    CHECK(w.dump() == first);
    CHECK_FALSE(w.taps.empty());
  }
}
}

TEST_CASE("a system snapshot with a frame in flight restores the same remaining frames")
{
  auto const build = single();
  auto const t     = reference(build)[0].at.ps;
  SUBCASE("slave frame running")
  {
    restored_run(build, t + latency + us_ps, [](world&) {});
  }
  SUBCASE("chip select held low between frames")
  {
    restored_run(build, t + 2 * latency + frame_ps + 2 * us_ps, [](world&) {});
  }
  SUBCASE("between a slave reset and the re-armed reply")
  {
    restored_run(build, t + latency + us_ps, [](world& w) { w.reset(1); });
  }
  SUBCASE("a frame in the M->S link at a slave reset")
  {
    restored_run(build, t + us_ps / 4, [](world& w) { w.reset(1); });
  }
  SUBCASE("the master muted by a reset at a pause, chip select low after it")
  {
    auto const again =
        single({cs_high(4), wait_us(50), cs_low(4), send(1, 0x10), send(1, 0x20), cs_high(4)});
    restored_run(again, t + 2 * us_ps, [](world& w) { w.reset(0); });
  }
}

TEST_CASE("an SPI link refuses another link kind's bytes, a UART wire refuses SPI bytes")
{
  auto                   w     = world{master_script(), {echo_slave(4)}};
  auto&                  forth = w.recorded[0]->inner();
  auto&                  back  = w.recorded[1]->inner();
  std::vector<std::byte> forth_bytes;
  std::vector<std::byte> back_bytes;
  forth.save(forth_bytes);
  back.save(back_bytes);
  CHECK_THROWS_AS(back.load(forth_bytes), popcorn::config_error);
  CHECK_THROWS_AS(forth.load(back_bytes), popcorn::config_error);
  auto* const usart = w.nodes[1]->f->model_of<stm32::uart_link_port>("USART1");
  REQUIRE(usart != nullptr);
  stm32::uart_wire wire{
      {.sender = 0, .receiver = 1},
      {.peer = 7, .injected = 8},
      core::sys_time{latency},
      {.baud = 115'200},
      {.port = usart, .unit = "USART1", .log = [](core::log_record) {}, .strict = false},
      nullptr
  };
  std::vector<std::byte> uart_bytes;
  wire.save(uart_bytes);
  CHECK_THROWS_AS(forth.load(uart_bytes), popcorn::config_error);
  CHECK_THROWS_AS(back.load(uart_bytes), popcorn::config_error);
  CHECK_THROWS_AS(wire.load(forth_bytes), popcorn::config_error);
  CHECK_THROWS_AS(wire.load(back_bytes), popcorn::config_error);
  std::vector<std::byte> after;
  forth.save(after);
  CHECK(after == forth_bytes);
  forth.load(forth_bytes);
  back.load(back_bytes);
}

TEST_CASE("the medium refuses a slave on the master's member and a slave with no port")
{
  auto       w   = world{master_script(), {echo_slave(4)}};
  auto const end = [&](std::size_t member, stm32::spi_link_port* port) {
    return std::vector<stm32::spi_slave_end>{
        {.member     = member,
         .port       = port,
         .cs_port    = "GPIOA",
         .cs_pin     = 4,
         .forth_lane = 1,
         .back_lane  = 1}
    };
  };
  CHECK_THROWS_WITH_AS(
      stm32::spi_medium(0, *w.nodes[0]->port, core::sys_time{latency}, end(0, w.nodes[1]->port)),
      "SPI slave 0 is an SPI on another member than the master's", std::invalid_argument);
  CHECK_THROWS_WITH_AS(
      stm32::spi_medium(0, *w.nodes[0]->port, core::sys_time{latency}, end(1, nullptr)),
      "SPI slave 0 has an SPI link port", std::invalid_argument);
}

TEST_CASE("1000 master resets after the reply's stamp keep at most one ticket")
{
  auto const build = single({kick(0x10), wait_us(30), reset_op(), repeat(1000, 0)});
  same_everywhere(
      "ticket bound", build, 600'000 * us_ps,
      [](world& w) {
        CHECK(w.frames_of(0).size() == 1000);
        CHECK(w.medium->tickets_peak(0) == 1);
        CHECK(w.medium->tickets(0) == 0);
        CHECK(w.refused() == 0);
  },
      {}, {{.name = "q=l"}, {.name = "q=l/2", .lowered = 2}});
}

TEST_CASE("an idle pause of sleeping members waits for the reply carried in the link")
{
  auto const                 build = single({cs_high(4), wait_us(50), cs_low(4), kick(0x10)});
  std::optional<std::string> first;
  for(auto const& how : variants())
  {
    CAPTURE(how.name);
    auto w = build();
    w.host(how);
    auto const stopped = w.idle();
    REQUIRE(stopped.reason == core::pause_reason::idle);
    auto const seen = w.frames_of(0);
    REQUIRE(seen.size() == 1);
    CHECK(seen[0].reply.ps == seen[0].at.ps + w.l + frame_ps);
    CHECK(stopped.at.ps >= seen[0].reply.ps + w.l);
    CHECK(w.medium->tickets(0) == 0);
    CHECK(w.medium->owed(0) == 0);
    CHECK(w.on(0, [](core::machine& target) {
      return popcorn::testing::saved_integer(target, "SPI1", "rx_fifo.0").has_value();
    }));
    auto const dump = std::format("{}idle at {}\n", w.dump(), stopped.at.ps);
    keep_dump(std::format("idle {}", how.name), dump);
    if(first)
    {
      CHECK(dump == *first);
    }
    else
    {
      first = dump;
    }
  }
}

TEST_CASE("an idle pause waits for the answer to a frame handed to a slave given back")
{
  auto const                 build = single({cs_high(4), wait_us(50), cs_low(4), kick(0x10)});
  auto const                 t     = reference(build)[0].at.ps;
  std::optional<std::string> first;
  for(auto const& how : variants())
  {
    CAPTURE(how.name);
    auto w = build();
    w.host(how);
    REQUIRE(w.run_to(t + us_ps / 4).reason == core::pause_reason::system_time);
    w.on(1, [&](core::machine&) { w.medium->give_back_slave(0); });
    auto const stopped = w.idle();
    REQUIRE(stopped.reason == core::pause_reason::idle);
    auto const seen = w.frames_of(0);
    REQUIRE(seen.size() == 1);
    CHECK(seen[0].dropped);
    CHECK(seen[0].reply.ps == t + w.l + frame_ps);
    CHECK(w.medium->tickets(0) == 0);
    CHECK(w.on(0, [](core::machine& target) {
      return popcorn::testing::saved_integer(target, "SPI1", "rx_fifo.0") == 0xFF;
    }));
    auto const dump = std::format("{}idle at {}\n", w.dump(), stopped.at.ps);
    if(first)
    {
      CHECK(dump == *first);
    }
    else
    {
      first = dump;
    }
  }
}

TEST_CASE("the links and proxies keep their slave's state alive after the medium is destroyed")
{
  auto w = single()();
  w.host({.name = "q=l"});
  w.medium.reset();
  REQUIRE(w.run_to(row_end).reason == core::pause_reason::system_time);
  CHECK(w.log(0) == std::vector<std::uint32_t>{boot, 0xA0, 0x20, 0x21});
  CHECK(w.log(1) == std::vector<std::uint32_t>{boot, 0x5110, 0x5111, 0x5112});
}

TEST_CASE("a frame owed by a slave given back gets all ones at its far-side end")
{
  auto const build = single({cs_high(4), wait_us(50), cs_low(4), send(1, 0x10), cs_high(4)});
  auto const t     = reference(build)[0].at.ps;
  auto const check = [&](world& w) {
    CHECK(w.log(0) == std::vector<std::uint32_t>{boot, 0xFF});
    CHECK(w.log(1) == std::vector<std::uint32_t>{boot});
    auto const seen = w.frames_of(0);
    REQUIRE(seen.size() == 1);
    CHECK(seen[0].dropped);
    CHECK(seen[0].reply.ps == t + w.l + frame_ps);
    CHECK(w.medium->owed(0) == 0);
    CHECK(w.medium->tickets(0) == 0);
    CHECK(w.refused() == 0);
  };
  auto const give_back = [](world& w) {
    w.on(1, [&](core::machine&) { w.medium->give_back_slave(0); });
  };
  SUBCASE("still in the M->S link")
  {
    same_everywhere(
        "give back slave link", build, row_end, check,
        {
            {.at = t + us_ps / 4, .act = give_back}
    });
  }
  SUBCASE("running in the slave's port")
  {
    same_everywhere(
        "give back slave port", build, row_end, check,
        {
            {.at = t + latency + frame_ps / 2, .act = give_back}
    });
  }
}

TEST_CASE("a frame drained to a slave given back keeps the M->S link loud until the S->M window")
{
  world w{
      {cs_high(4), wait_us(50), cs_low(4), kick(0x10)},
      {echo_slave(4)}
  };
  auto const links = w.medium->links();
  REQUIRE(links.size() == 2);
  auto& forth = *links[0];
  auto& back  = *links[1];
  forth.enter_window(0, core::time_map{});
  back.enter_window(0, core::time_map{});
  auto const& master = *w.nodes[0]->f;
  auto const  end    = vtime{static_cast<std::uint64_t>(w.nodes[0]->origin + 200 * us_ps)};
  REQUIRE(
      master.run_to_stop({.max_instructions = icount{1'000'000}, .until_time = end}).reason
      == stop_reason::time);
  REQUIRE_FALSE(forth.quiet());
  w.medium->give_back_slave(0);
  auto const barrier = core::sys_time{static_cast<std::int64_t>(end.ps)};
  auto const next    = core::sys_time{barrier.ps + row_end};
  CHECK_FALSE(forth.drain(0, barrier, next, core::time_map{}).has_value());
  CHECK_FALSE(forth.quiet());
  CHECK(back.quiet());
  back.enter_window(1, core::time_map{});
  CHECK(forth.quiet());
  CHECK_FALSE(back.quiet());
  CHECK_FALSE(back.drain(1, next, core::sys_time{next.ps + row_end}, core::time_map{}).has_value());
  CHECK(back.quiet());
  CHECK(w.medium->owed(0) == 0);
}

namespace
{
constexpr std::int64_t slow_l     = 100 * us_ps;
constexpr std::int64_t slow_ps    = 2'048 * us_ps;
constexpr std::int64_t fast_ps    = 2 * us_ps;
constexpr std::int64_t resent_end = 7'000 * us_ps;

struct resent
{
  build_fn     build;
  std::int64_t t1{};
  std::int64_t t2{};
  std::int64_t reset{};
  std::int64_t handed{};
};

auto resent_after_rcc() -> resent
{
  for(std::uint32_t wait = 50; wait < 200; wait += 7)
  {
    auto const script = std::vector<std::uint32_t>{
        cs_high(4),     wait_us(wait), cs_low(4),       format_op(32, 7), kick(0x5A'5A5A),
        wait_us(2'058), rcc_op(),      format_op(4, 0), send(1, 0x5)};
    build_fn const build = [script] { return world{script, {echo_slave(4)}, slow_l}; };
    auto const     seen  = reference(build, resent_end);
    REQUIRE(seen.size() == 2);
    auto const t1    = seen[0].at.ps;
    auto const t2    = seen[1].at.ps;
    auto const reset = try_pick(t1 + slow_l, t1 + slow_l + slow_ps, [&](std::int64_t x) {
      return handed_by(t2, x - us_ps, slow_l, slow_l) and not handed_by(t2, x, slow_l, slow_l / 2);
    });
    if(reset)
    {
      return {.build = build, .t1 = t1, .t2 = t2, .reset = *reset, .handed = *reset - us_ps / 2};
    }
  }
  FAIL("no wait puts the slave reset between the two drains");
  return {};
}

void one_reply_per_unit(world& w, resent const& r)
{
  auto const seen = w.frames_of(0);
  REQUIRE(seen.size() == 2);
  CHECK(seen[0].unit < seen[1].unit);
  CHECK(seen[0].dropped);
  CHECK(seen[0].bits == 32);
  CHECK(seen[0].mosi == 0x5A'5A5A);
  CHECK(seen[0].reply.ps == r.t1 + w.l + slow_ps);
  CHECK_FALSE(seen[1].dropped);
  CHECK(seen[1].bits == 4);
  CHECK(seen[1].mosi == 0x5);
  CHECK(seen[1].reply.ps == r.t2 + w.l + fast_ps);
  CHECK(w.log(0) == std::vector<std::uint32_t>{boot, seen[1].miso});
  CHECK(w.refused() == 1);
  CHECK(w.medium->owed(0) == 0);
  CHECK(w.medium->tickets(0) == 0);
}
}

TEST_CASE("a frame resent after a master RCC reset gets its own reply across a slave RCC reset")
{
  auto const r = resent_after_rcc();
  same_everywhere(
      "resent", r.build, resent_end,
      [&](world& w) {
        one_reply_per_unit(w, r);
  },
      {{.at = r.reset, .act = [](world& w) { w.rcc(1); }}});
}

TEST_CASE("a frame kept in the slave port over an RCC reset restores with its own start")
{
  auto const r = resent_after_rcc();
  for(auto const& how : variants())
  {
    CAPTURE(how.name);
    auto w = r.build();
    w.host(how);
    REQUIRE(w.run_to(r.handed).reason == core::pause_reason::system_time);
    auto const saved = capture(w);
    auto const rest  = [&] {
      REQUIRE(w.run_to(r.reset).reason == core::pause_reason::system_time);
      w.rcc(1);
      REQUIRE(w.run_to(resent_end).reason == core::pause_reason::system_time);
      return w.dump();
    };
    auto const first = rest();
    one_reply_per_unit(w, r);
    put_back(w, saved);
    w.taps.clear();
    CHECK(rest() == first);
  }
}
