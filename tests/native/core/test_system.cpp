#include <popcorn/core/errors.hpp>
#include <popcorn/core/host.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/core/member_link.hpp>
#include <popcorn/core/system.hpp>
#include <popcorn/core/window_barrier.hpp>

#include "../support/machine_fixture.hpp"
#include "../support/test_arch.hpp"

#include <doctest/doctest.h>

#include <sched.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace
{
namespace core = popcorn::core;
using popcorn::icount;
using popcorn::stop_reason;
using popcorn::vtime;
using popcorn::testing::m33;
using popcorn::testing::sym;

constexpr std::int64_t us_ps = 1'000'000;
constexpr std::int64_t ms_ps = 1'000'000'000;

struct test_failure : std::runtime_error
{
  using std::runtime_error::runtime_error;
};

class throwing_arch final : public popcorn::testing::test_arch
{
public:
  using test_arch::test_arch;

  static constexpr std::uint32_t throw_event = 9;

  void on_event(std::uint32_t kind, std::uint64_t payload) override
  {
    if(kind == throw_event)
    {
      throw test_failure("member event");
    }
    test_arch::on_event(kind, payload);
  }
};

struct drained
{
  std::uint64_t window{};
  std::int64_t  barrier{};
  std::int64_t  next{};

  friend auto operator==(drained const&, drained const&) -> bool = default;
};

class fake_link final : public core::member_link
{
public:
  static constexpr std::size_t kept_drains = 4096;

  fake_link(std::size_t from, std::size_t to, std::int64_t latency)
      : from_{from}, to_{to}, latency_{latency}
  {}

  auto sender() const noexcept -> std::size_t override
  {
    return from_;
  }

  auto receiver() const noexcept -> std::size_t override
  {
    return to_;
  }

  auto latency() const noexcept -> core::sys_time override
  {
    return core::sys_time{latency_};
  }

  void enter_window(std::uint64_t window, core::time_map) override
  {
    entered = window;
  }

  void sender_reset(core::sys_time at) override
  {
    sender_resets.push_back(at.ps);
  }

  void receiver_reset(core::sys_time at) override
  {
    receiver_resets.push_back(at.ps);
  }

  auto drain(std::uint64_t window, core::sys_time barrier, core::sys_time next, core::time_map)
      -> std::optional<core::link_fault> override
  {
    if(drains.size() < kept_drains)
    {
      drains.push_back({.window = window, .barrier = barrier.ps, .next = next.ps});
    }
    if(on_drain)
    {
      on_drain(barrier.ps);
    }
    if(fault_at and not reported and *fault_at < barrier.ps)
    {
      reported = true;
      return core::link_fault{
          .at     = core::sys_time{*fault_at},
          .member = to_,
          .error  = std::make_exception_ptr(test_failure("link fault"))};
    }
    return std::nullopt;
  }

  auto quiet() const -> bool override
  {
    return not busy;
  }

  void finish(core::sys_time at) override
  {
    finished.push_back(at.ps);
  }

  void save(std::vector<std::byte>&) const override
  {}

  void load(std::span<std::byte const>) override
  {}

  std::uint64_t                     entered{};
  std::vector<drained>              drains;
  std::vector<std::int64_t>         sender_resets;
  std::vector<std::int64_t>         receiver_resets;
  std::optional<std::int64_t>       fault_at{};
  std::function<void(std::int64_t)> on_drain{};
  bool                              busy{};
  std::vector<std::int64_t>         finished{};
  bool                              reported{};

private:
  std::size_t  from_;
  std::size_t  to_;
  std::int64_t latency_;
};

template <typename Arch = popcorn::testing::test_arch>
auto member(
    std::string_view                                              firmware,
    std::string_view                                              entry,
    std::function<void(popcorn::testing::machine_fixture<Arch>&)> prepare = {}) -> core::member_ref
{
  popcorn::testing::machine_fixture<Arch> f{m33(), firmware, core::machine_options{}};
  f.set("pc", sym(firmware, entry));
  f.set("r0", 0);
  if(prepare)
  {
    prepare(f);
  }
  std::vector<std::unique_ptr<core::machine>> machines;
  machines.push_back(std::move(f.target));
  return {.on = std::make_shared<core::host>(std::move(machines)), .name = std::string{entry}};
}

struct variant
{
  std::string_view             name;
  std::int64_t                 lowered{};
  core::slice_bounds           bounds{};
  std::optional<std::uint32_t> seed{};
  bool                         one_cpu{};
};

auto variants() -> std::vector<variant>
{
  return {
      {.name = "q=l"},
      {.name = "q=l/3", .lowered = 3},
      {.name   = "997",
       .bounds = {.max_instructions = icount{997}, .wall_budget = std::chrono::microseconds{10}}},
      {.name    = "997 q=l/3",
       .lowered = 3,
       .bounds  = {.max_instructions = icount{997}, .wall_budget = std::chrono::microseconds{10}}},
      {.name = "sleeps 1", .seed = 1},
      {.name = "sleeps 2", .lowered = 3, .seed = 2},
      {.name = "one cpu", .one_cpu = true},
  };
}

class pinned
{
public:
  explicit pinned(bool on) : on_{on}
  {
    if(not on_)
    {
      return;
    }
    REQUIRE(sched_getaffinity(0, sizeof saved_, &saved_) == 0);
    cpu_set_t one;
    CPU_ZERO(&one);
    for(int cpu = 0; cpu < CPU_SETSIZE; ++cpu)
    {
      if(CPU_ISSET(cpu, &saved_))
      {
        CPU_SET(cpu, &one);
        break;
      }
    }
    REQUIRE(sched_setaffinity(0, sizeof one, &one) == 0);
  }

  pinned(pinned const&)                    = delete;
  auto operator=(pinned const&) -> pinned& = delete;

  ~pinned()
  {
    if(on_)
    {
      sched_setaffinity(0, sizeof saved_, &saved_);
    }
  }

private:
  bool      on_;
  cpu_set_t saved_{};
};

struct rig
{
  std::vector<core::member_ref>           members;
  std::vector<std::shared_ptr<fake_link>> links{};
  std::unique_ptr<core::system>           sys{};

  void build(variant const& how, std::int64_t latency = 20 * us_ps)
  {
    std::vector<std::shared_ptr<core::member_link>> wires;
    links.push_back(std::make_shared<fake_link>(0, 1, latency));
    if(how.lowered != 0)
    {
      links.push_back(std::make_shared<fake_link>(1, 0, latency / how.lowered));
    }
    for(auto const& link : links)
    {
      wires.push_back(link);
    }
    std::function<void(std::size_t)> hook;
    if(how.seed)
    {
      auto engines = std::make_shared<std::vector<std::mt19937>>();
      for(std::size_t index = 0; index < members.size(); ++index)
      {
        engines->emplace_back(*how.seed * 31 + index);
      }
      hook = [engines](std::size_t index) {
        std::uniform_int_distribution<int> pause{0, 50};
        std::this_thread::sleep_for(std::chrono::microseconds{pause((*engines)[index])});
      };
    }
    auto const wait = how.one_cpu ? core::wait_policy{.spin = std::chrono::nanoseconds{0}}
                                  : core::default_wait_policy(members.size());
    sys = std::make_unique<core::system>(members, std::move(wires), wait, how.bounds, hook);
    auto const                  held = sys->lock();
    std::vector<core::sys_time> offsets(members.size());
    sys->start(held, offsets);
  }

  auto run(std::optional<std::int64_t> until) -> core::pause
  {
    core::host_future<core::pause> pending;
    {
      auto const held = sys->lock();
      pending = sys->resume(held, until ? std::optional{core::sys_time{*until}} : std::nullopt);
    }
    return pending.get();
  }

  auto goal(std::size_t member, core::member_goal wanted) -> core::host_future<core::goal_reached>
  {
    auto const held = sys->lock();
    return sys->post_goal(held, member, std::move(wanted));
  }

  void release(std::size_t member)
  {
    auto const held = sys->lock();
    sys->release(held, member);
  }

  [[nodiscard]] auto now_of(std::size_t member) const -> std::int64_t
  {
    auto const held = sys->lock();
    return sys->time_of(held, member).ps;
  }

  [[nodiscard]] auto dump() const -> std::vector<std::uint64_t>
  {
    std::vector<std::uint64_t> out;
    auto const                 held = sys->lock();
    for(std::size_t index = 0; index < members.size(); ++index)
    {
      out.push_back(static_cast<std::uint64_t>(sys->time_of(held, index).ps));
      core::access(*members[index].on, 0, [&out](core::machine& m) {
        for(std::string_view const name :
            {"r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7", "r12", "sp", "lr", "pc"})
        {
          out.push_back(m.regs().read(*m.regs().find(name)));
        }
        out.push_back(m.virtual_clock().now().value);
        out.push_back(m.pending_events());
      });
    }
    return out;
  }
};

auto sleeper_and_runner() -> std::vector<core::member_ref>
{
  return {member("idle", "idle_budget"), member("alu", "alu_entry")};
}

auto parked(core::member_ref const& one) -> bool
{
  auto const held = one.on->lock();
  return one.on->parked(held, 0);
}
}

TEST_CASE("a system TIME run leaves a sleeper exactly at T and a runner at its block past it")
{
  constexpr std::int64_t                    until = 1 * ms_ps + 3 * us_ps + 250'000;
  std::optional<std::vector<std::uint64_t>> reference;
  for(auto const& how : variants())
  {
    CAPTURE(how.name);
    pinned const pin{how.one_cpu};
    rig          r{
        .members = {member("idle", "idle_forever"), member("alu", "alu_entry")}
    };
    r.build(how);
    auto const stopped = r.run(until);
    CHECK(stopped.reason == core::pause_reason::system_time);
    {
      auto const held = r.sys->lock();
      CHECK(r.sys->time_of(held, 0).ps == until);
      auto const runner = r.sys->time_of(held, 1).ps;
      CHECK(runner >= until);
      CHECK(runner < until + 512 * 250'000);
      CHECK(stopped.at.ps == runner);
      CHECK(stopped.member == (runner > until ? 1 : 0));
    }
    CHECK(parked(r.members[0]));
    CHECK(parked(r.members[1]));
    auto const dump = r.dump();
    if(reference)
    {
      CHECK(dump == *reference);
    }
    else
    {
      reference = dump;
    }
  }
}

TEST_CASE("chunked system TIME runs equal one run to the last T")
{
  for(auto const& how : {variants()[0], variants()[3]})
  {
    CAPTURE(how.name);
    rig whole{.members = sleeper_and_runner()};
    whole.build(how);
    CHECK(whole.run(3 * ms_ps + 7 * us_ps).reason == core::pause_reason::system_time);
    rig chunked{.members = sleeper_and_runner()};
    chunked.build(how);
    CHECK(chunked.run(1 * ms_ps + 401'000).reason == core::pause_reason::system_time);
    CHECK(chunked.run(2 * ms_ps + 13 * us_ps).reason == core::pause_reason::system_time);
    CHECK(chunked.run(3 * ms_ps + 7 * us_ps).reason == core::pause_reason::system_time);
    CHECK(chunked.dump() == whole.dump());
  }
}

TEST_CASE("every receiver drains each window once, in order, at its barrier")
{
  rig r{.members = sleeper_and_runner()};
  r.build(variants()[1]);
  auto const stopped = r.run(1 * ms_ps);
  REQUIRE(stopped.reason == core::pause_reason::system_time);
  auto const q = r.sys->quantum().ps;
  CHECK(q == 20 * us_ps / 3);
  for(auto const& link : r.links)
  {
    CAPTURE(link->receiver());
    REQUIRE(link->drains.size() == stopped.windows + 1);
    for(std::uint64_t window = 0; window < stopped.windows; ++window)
    {
      CHECK(
          link->drains[window]
          == drained{
              .window  = window,
              .barrier = static_cast<std::int64_t>(window + 1) * q,
              .next    = static_cast<std::int64_t>(window + 2) * q});
    }
    auto const& confirm = link->drains.back();
    CHECK(confirm.window == stopped.windows);
    CHECK(confirm.barrier == 1 * ms_ps);
    CHECK(confirm.next == 1 * ms_ps);
  }
  auto const held = r.sys->lock();
  CHECK(r.sys->pending_barrier(held).ps == static_cast<std::int64_t>(stopped.windows + 1) * q);
}

TEST_CASE("offsets set each member's system time and a reset while paused keeps it")
{
  rig r{.members = sleeper_and_runner()};
  r.links.push_back(std::make_shared<fake_link>(0, 1, 20 * us_ps));
  r.sys = std::make_unique<core::system>(
      r.members, std::vector<std::shared_ptr<core::member_link>>{r.links[0]},
      core::default_wait_policy(2));
  {
    auto const                        held = r.sys->lock();
    std::vector<core::sys_time> const offsets{core::sys_time{0}, core::sys_time{37 * us_ps}};
    r.sys->start(held, offsets);
    CHECK(r.sys->time_of(held, 1).ps == 37 * us_ps);
  }
  REQUIRE(r.run(500 * us_ps).reason == core::pause_reason::system_time);
  auto const held   = r.sys->lock();
  auto const before = r.sys->time_of(held, 0);
  r.sys->reset(held, 0, popcorn::reset_kind::software);
  CHECK(r.sys->time_of(held, 0) == before);
  CHECK(r.links[0]->sender_resets == std::vector{before.ps});
  CHECK(r.links[0]->receiver_resets.empty());
}

TEST_CASE("a pause requested after its flight ended does not end the next flight")
{
  rig r{
      .members = {member("alu", "alu_entry"), member("alu", "alu_entry")}
  };
  r.build(variants()[0]);
  REQUIRE(r.run(100 * us_ps).reason == core::pause_reason::system_time);
  r.sys->request_pause();
  CHECK(r.run(200 * us_ps).reason == core::pause_reason::system_time);
}

TEST_CASE("a pause requested right after resume ends that flight")
{
  rig r{
      .members = {member("alu", "alu_entry"), member("alu", "alu_entry")}
  };
  r.build(variants()[0]);
  core::host_future<core::pause> pending;
  {
    auto const held = r.sys->lock();
    pending         = r.sys->resume(held, core::sys_time{10 * ms_ps});
  }
  r.sys->request_pause();
  CHECK(pending.get().reason == core::pause_reason::requested);
}

TEST_CASE("a reset done behind the system's back fails the next resume")
{
  rig r{.members = sleeper_and_runner()};
  r.build(variants()[0]);
  REQUIRE(r.run(200 * us_ps).reason == core::pause_reason::system_time);
  core::access(*r.members[1].on, 0, [](core::machine& m) { m.reset(); });
  auto const held = r.sys->lock();
  CHECK_THROWS_AS(
      std::ignore = r.sys->resume(held, core::sys_time{400 * us_ps}), popcorn::emulation_error);
}

TEST_CASE("a member's event that throws pauses the system on that error at its time")
{
  std::optional<core::sys_time> reference;
  for(auto const& how : variants())
  {
    CAPTURE(how.name);
    pinned const pin{how.one_cpu};
    rig          r{
        .members = {
                    member("alu", "alu_entry"),
                    member<throwing_arch>(
                "idle", "idle_budget", [](popcorn::testing::machine_fixture<throwing_arch>& f) {
                  std::ignore = f.module->attached_events().schedule(
                      icount{1777}, throwing_arch::throw_event, 0);
                })}
    };
    r.build(how);
    auto const stopped = r.run(2 * ms_ps);
    REQUIRE(stopped.reason == core::pause_reason::error);
    CHECK(stopped.member == 1);
    CHECK_THROWS_AS(std::rethrow_exception(stopped.error), test_failure);
    CHECK(stopped.at.ps == 1777 * 250'000);
    CHECK(parked(r.members[0]));
    CHECK(parked(r.members[1]));
    if(reference)
    {
      CHECK(stopped.at == *reference);
    }
    reference       = stopped.at;
    auto const held = r.sys->lock();
    CHECK_THROWS_AS(std::ignore = r.sys->resume(held, core::sys_time{3 * ms_ps}), std::logic_error);
  }
}

TEST_CASE("a link fault only the pause's drain finds comes before the entry it precedes")
{
  for(auto const lowered : {std::int64_t{0}, std::int64_t{3}})
  {
    CAPTURE(lowered);
    rig r{.members = sleeper_and_runner()};
    r.build({.name = "fault", .lowered = lowered});
    r.links[0]->fault_at = 1 * ms_ps + 2 * us_ps;
    auto const stopped   = r.run(1 * ms_ps + 4 * us_ps);
    REQUIRE(stopped.reason == core::pause_reason::error);
    CHECK(stopped.at.ps == 1 * ms_ps + 2 * us_ps);
    CHECK(stopped.member == 1);
  }
}

TEST_CASE("faults found at different barriers are delivered by time, at any quantum")
{
  for(auto const lowered : {std::int64_t{0}, std::int64_t{3}})
  {
    CAPTURE(lowered);
    rig r{.members = sleeper_and_runner()};
    r.build({.name = "faults", .lowered = lowered});
    r.links[0]->fault_at = 300 * us_ps + 1;
    if(lowered != 0)
    {
      r.links[1]->fault_at = 300 * us_ps + 2;
    }
    auto const stopped = r.run(2 * ms_ps);
    REQUIRE(stopped.reason == core::pause_reason::error);
    CHECK(stopped.at.ps == 300 * us_ps + 1);
    CHECK(stopped.member == 1);
  }
}

TEST_CASE("halted members advance with their events and pause at the barrier on request")
{
  rig r{.members = sleeper_and_runner()};
  r.build(variants()[0]);
  REQUIRE(r.run(200 * us_ps).reason == core::pause_reason::system_time);
  auto const                     before = r.dump();
  core::host_future<core::pause> pending;
  {
    auto const held = r.sys->lock();
    pending         = r.sys->resume(held);
  }
  std::this_thread::sleep_for(std::chrono::milliseconds{20});
  r.sys->request_pause();
  auto const stopped = pending.get();
  CHECK(stopped.reason == core::pause_reason::requested);
  CHECK(stopped.windows > 0);
  auto const held = r.sys->lock();
  CHECK(r.sys->time_of(held, 1) == stopped.at);
  CHECK(r.sys->pending_barrier(held) == stopped.at);
  core::access(*r.members[1].on, 0, [&](core::machine& m) {
    CHECK(m.core_held());
    CHECK(m.regs().read(*m.regs().find("pc")) == before[27]);
  });
}

TEST_CASE("a member's time and the pending barrier are refused while a resume is in flight")
{
  rig r{.members = sleeper_and_runner()};
  r.build(variants()[0]);
  CHECK(r.sys->hosts() == std::vector{r.members[0].on, r.members[1].on});
  core::host_future<core::pause> pending;
  {
    auto const held = r.sys->lock();
    pending         = r.sys->resume(held, core::sys_time{10'000 * ms_ps});
    CHECK_THROWS_AS(std::ignore = r.sys->time_of(held, 0), popcorn::machine_running);
    CHECK_THROWS_AS(std::ignore = r.sys->pending_barrier(held), popcorn::machine_running);
  }
  r.sys->request_pause();
  auto const stopped = pending.get();
  CHECK(stopped.reason == core::pause_reason::requested);
  auto const held = r.sys->lock();
  CHECK(r.sys->time_of(held, 0) == stopped.at);
  CHECK(r.sys->pending_barrier(held) == stopped.at);
}

TEST_CASE("close while members run and wait parks every member and ends the pause as closed")
{
  for(int round = 0; round < 50; ++round)
  {
    CAPTURE(round);
    rig r{
        .members = {
                    member("alu", "alu_entry"), member("alu", "alu_entry"), member("idle", "idle_budget")}
    };
    r.links.push_back(std::make_shared<fake_link>(0, 1, 20 * us_ps));
    r.sys = std::make_unique<core::system>(
        r.members, std::vector<std::shared_ptr<core::member_link>>{r.links[0]},
        core::default_wait_policy(3));
    core::host_future<core::pause> pending;
    {
      auto const                  held = r.sys->lock();
      std::vector<core::sys_time> offsets(3);
      r.sys->start(held, offsets);
      pending = r.sys->resume(held, core::sys_time{10'000 * ms_ps});
    }
    std::this_thread::sleep_for(std::chrono::microseconds{round * 37});
    r.sys->close();
    CHECK(pending.get().reason == core::pause_reason::closed);
    for(auto const& one : r.members)
    {
      CHECK(parked(one));
    }
    auto const held = r.sys->lock();
    CHECK_THROWS_AS(std::ignore = r.sys->resume(held), core::machine_closed);
  }
}

namespace
{
constexpr popcorn::address scratch_code = 0x2004'0000;
constexpr std::uint16_t    thumb_nop    = 0xbf00;
constexpr std::uint16_t    branch_self  = 0xe7fe;
constexpr std::uint32_t    logged_kind  = 1;

struct long_block
{
  popcorn::testing::test_arch* module{};
  core::machine*               target{};
  vtime                        start{};
};

auto long_block_member(long_block& made) -> core::member_ref
{
  return member<popcorn::testing::test_arch>(
      "calls", "spin_forever", [&made](popcorn::testing::machine_fixture<>& f) {
        std::vector<std::uint16_t> code(400, thumb_nop);
        code.push_back(branch_self);
        for(std::size_t index = 0; index < code.size(); ++index)
        {
          REQUIRE(f.target->mem().write(scratch_code + 2 * index, 2, code[index]));
        }
        f.set("pc", scratch_code);
        made.module    = f.module;
        made.target    = f.target.get();
        made.start     = f.target->virtual_clock().time();
        auto const now = f.target->virtual_clock().now().value;
        std::ignore    = f.module->attached_events().schedule(icount{now + 100}, logged_kind, 1);
        std::ignore    = f.module->attached_events().schedule(icount{now + 300}, logged_kind, 3);
      });
}
}

TEST_CASE("a lane event drained into a block held past the barrier fires in key order")
{
  constexpr std::int64_t arrival = 200 * 250'000;
  for(auto const& how : variants())
  {
    CAPTURE(how.name);
    pinned const pin{how.one_cpu};
    long_block   made;
    rig          r{
        .members = {member("alu", "alu_entry"), long_block_member(made)}
    };
    r.build(how);
    r.links[0]->on_drain = [&made, posted = false](std::int64_t barrier) mutable {
      if(posted or barrier <= arrival - 20 * us_ps)
      {
        return;
      }
      posted      = true;
      std::ignore = made.module->attached_events().schedule_at(
          vtime{made.start.ps + static_cast<std::uint64_t>(arrival)}, logged_kind, 50, 1,
          made.target->virtual_clock());
    };
    REQUIRE(r.run(300 * us_ps).reason == core::pause_reason::system_time);
    std::vector<std::uint64_t> order;
    core::access(*r.members[1].on, 0, [&](core::machine&) {
      for(auto const& one : made.module->event_log)
      {
        order.push_back(one.payload);
      }
    });
    CHECK(order == std::vector<std::uint64_t>{1, 50, 3});
  }
}

TEST_CASE("a TIME run whose T lies in a block past several barriers dispatches up to T only")
{
  std::optional<std::vector<std::uint64_t>> reference;
  for(auto const latency : {20 * us_ps, 200 * us_ps})
  {
    for(auto const& how : {variants()[0], variants()[1], variants()[2]})
    {
      CAPTURE(latency);
      CAPTURE(how.name);
      long_block made;
      rig        r{
          .members = {member("alu", "alu_entry"), long_block_member(made)}
      };
      r.build(how, latency);
      REQUIRE(r.run(60 * us_ps).reason == core::pause_reason::system_time);
      std::vector<std::uint64_t> order;
      core::access(*r.members[1].on, 0, [&](core::machine&) {
        for(auto const& one : made.module->event_log)
        {
          order.push_back(one.payload);
        }
      });
      CHECK(order == std::vector<std::uint64_t>{1});
      auto dump = r.dump();
      dump.insert(dump.end(), order.begin(), order.end());
      if(reference)
      {
        CHECK(dump == *reference);
      }
      reference = dump;
    }
  }
}

namespace
{
auto alu_counting(std::uint32_t loops) -> core::member_ref
{
  return member<popcorn::testing::test_arch>(
      "alu", "alu_entry", [loops](popcorn::testing::machine_fixture<>& f) { f.set("r0", loops); });
}

struct delivery
{
  int          reason{};
  std::size_t  member{};
  std::int64_t at{};

  friend auto operator==(delivery const&, delivery const&) -> bool = default;
};

auto delivered(core::pause const& one) -> delivery
{
  return {
      .reason = static_cast<int>(one.reason), .member = one.member.value_or(99), .at = one.at.ps};
}
}

TEST_CASE("goals to two addresses deliver each at its point, the released one halted after")
{
  std::optional<std::pair<std::vector<std::uint64_t>, std::vector<delivery>>> reference;
  for(auto const& how : variants())
  {
    CAPTURE(how.name);
    pinned const pin{how.one_cpu};
    rig          r{
        .members = {alu_counting(2000), alu_counting(6000)}
    };
    r.build(how);
    auto const            done = sym("alu", "alu_done");
    auto                  a    = r.goal(0, {.until = done});
    auto                  b    = r.goal(1, {.until = done});
    std::vector<delivery> order;
    auto const            first = r.run({});
    order.push_back(delivered(first));
    REQUIRE(first.reason == core::pause_reason::delivered);
    CHECK(first.member == 0);
    REQUIRE(a.done());
    CHECK(a.get().stopped.reason == stop_reason::address);
    CHECK(a.get().at == first.at);
    CHECK_FALSE(b.done());
    {
      auto const held = r.sys->lock();
      CHECK(r.sys->undecided(held) == std::vector<std::size_t>{0});
      CHECK_THROWS_AS(std::ignore = r.sys->resume(held), std::logic_error);
    }
    r.release(0);
    auto const second = r.run({});
    order.push_back(delivered(second));
    REQUIRE(second.member == 1);
    CHECK(b.get().stopped.reason == stop_reason::address);
    CHECK(r.now_of(0) == second.at.ps);
    r.release(1);
    {
      auto const held = r.sys->lock();
      r.sys->finish(held);
      CHECK_THROWS_AS(std::ignore = r.sys->resume(held), std::logic_error);
    }
    core::access(*r.members[0].on, 0, [](core::machine& m) { CHECK_FALSE(m.core_held()); });
    std::pair seen{r.dump(), order};
    if(reference)
    {
      CHECK(seen == *reference);
    }
    reference = seen;
  }
}

TEST_CASE("two goals resolved in one window deliver by time, then by member index")
{
  for(auto const& how : {variants()[0], variants()[1], variants()[4]})
  {
    CAPTURE(how.name);
    rig r{
        .members = {member("alu", "alu_entry"), member("alu", "alu_entry")}
    };
    r.build(how);
    auto       a     = r.goal(0, {.budget = icount{150}});
    auto       b     = r.goal(1, {.budget = icount{100}});
    auto const first = r.run({});
    CHECK(first.member == 1);
    r.release(1);
    auto const second = r.run({});
    CHECK(second.member == 0);
    CHECK(first.at < second.at);

    rig tie{
        .members = {member("alu", "alu_entry"), member("alu", "alu_entry")}
    };
    tie.build(how);
    auto       c     = tie.goal(0, {.budget = icount{100}});
    auto       d     = tie.goal(1, {.budget = icount{100}});
    auto const lower = tie.run({});
    CHECK(lower.member == 0);
    tie.release(0);
    auto const upper = tie.run({});
    CHECK(upper.member == 1);
    CHECK(upper.at == lower.at);
  }
}

TEST_CASE("a goal re-issued at its delivered point resolves before a later entry already found")
{
  std::optional<std::vector<delivery>> reference;
  for(auto const& how : variants())
  {
    CAPTURE(how.name);
    pinned const pin{how.one_cpu};
    rig          r{
        .members = {
                    member("alu", "alu_entry"), member("alu", "alu_entry"), member("alu", "alu_entry")}
    };
    r.links.push_back(std::make_shared<fake_link>(0, 1, 20 * us_ps / (how.lowered == 0 ? 1 : 3)));
    r.sys = std::make_unique<core::system>(
        r.members, std::vector<std::shared_ptr<core::member_link>>{r.links[0]},
        core::default_wait_policy(3), how.bounds);
    {
      auto const                  held = r.sys->lock();
      std::vector<core::sys_time> offsets(3);
      r.sys->start(held, offsets);
    }
    auto                  a = r.goal(0, {.budget = icount{100}});
    auto                  b = r.goal(1, {.budget = icount{150}});
    auto                  c = r.goal(2, {.budget = icount{400}});
    std::vector<delivery> order;
    order.push_back(delivered(r.run({})));
    auto const executed = core::access(
        *r.members[0].on, 0, [](core::machine& m) { return m.virtual_clock().now().value; });
    std::this_thread::sleep_for(std::chrono::milliseconds{20});
    auto again = r.goal(0, {.budget = icount{20}});
    order.push_back(delivered(r.run({})));
    CHECK(
        core::access(
            *r.members[0].on, 0, [](core::machine& m) { return m.virtual_clock().now().value; })
        >= executed + 20);
    r.release(0);
    order.push_back(delivered(r.run({})));
    r.release(1);
    order.push_back(delivered(r.run({})));
    REQUIRE(order.size() == 4);
    CHECK(order[0].member == 0);
    CHECK(order[1].member == 0);
    CHECK(order[2].member == 1);
    CHECK(order[3].member == 2);
    CHECK(order[0].at < order[1].at);
    CHECK(order[1].at < order[2].at);
    if(reference)
    {
      CHECK(order == *reference);
    }
    reference = order;
  }
}

TEST_CASE("a goal posted to a halted member while paused starts at the delivered point")
{
  for(auto const& how : {variants()[0], variants()[1]})
  {
    CAPTURE(how.name);
    rig r{
        .members = {member("alu", "alu_entry"), member("alu", "alu_entry")}
    };
    r.build(how);
    auto       a     = r.goal(0, {.budget = icount{777}});
    auto const first = r.run({});
    REQUIRE(first.member == 0);
    CHECK(r.now_of(1) == first.at.ps);
    auto const before = core::access(
        *r.members[1].on, 0, [](core::machine& m) { return m.virtual_clock().now().value; });
    auto c = r.goal(1, {.budget = icount{10}});
    r.release(0);
    auto const second = r.run({});
    REQUIRE(second.member == 1);
    CHECK(c.get().stopped.reason == stop_reason::budget);
    CHECK(
        core::access(
            *r.members[1].on, 0, [](core::machine& m) { return m.virtual_clock().now().value; })
            - before
        < 20);
  }
}

TEST_CASE("goal stops past a barrier dispatch the lane event before them at any quantum")
{
  struct shape
  {
    std::string_view  name;
    core::member_goal goal;
    stop_reason       reason;
  };
  std::vector<shape> const shapes{
      {"budget",  {.budget = icount{300}},                    stop_reason::budget },
      {"address", {.until = scratch_code + 2 * 400},          stop_reason::address},
      {"time",    {.until_time = core::sys_time{60 * us_ps}}, stop_reason::time   },
  };
  constexpr std::int64_t arrival = 200 * 250'000;
  for(auto const& [name, wanted, reason] : shapes)
  {
    std::optional<std::vector<std::uint64_t>> reference;
    for(auto const latency : {20 * us_ps, 200 * us_ps})
    {
      for(auto const& how : {variants()[0], variants()[1]})
      {
        CAPTURE(name);
        CAPTURE(latency);
        CAPTURE(how.name);
        long_block made;
        rig        r{
            .members = {member("alu", "alu_entry"), long_block_member(made)}
        };
        r.build(how, latency);
        r.links[0]->on_drain = [&made, posted = false](std::int64_t barrier) mutable {
          if(posted or barrier <= arrival - 20 * us_ps)
          {
            return;
          }
          posted      = true;
          std::ignore = made.module->attached_events().schedule_at(
              vtime{made.start.ps + static_cast<std::uint64_t>(arrival)}, logged_kind, 50, 1,
              made.target->virtual_clock());
        };
        if(latency > 20 * us_ps)
        {
          std::ignore          = core::access(*r.members[1].on, 0, [&](core::machine&) {
            return made.module->attached_events().schedule_at(
                vtime{made.start.ps + static_cast<std::uint64_t>(arrival)}, logged_kind, 50, 1,
                made.target->virtual_clock());
          });
          r.links[0]->on_drain = {};
        }
        auto       goal    = r.goal(1, wanted);
        auto const stopped = r.run({});
        REQUIRE(stopped.member == 1);
        CHECK(goal.get().stopped.reason == reason);
        std::vector<std::uint64_t> order;
        core::access(*r.members[1].on, 0, [&](core::machine&) {
          for(auto const& one : made.module->event_log)
          {
            order.push_back(one.payload);
          }
        });
        order.push_back(static_cast<std::uint64_t>(stopped.at.ps));
        if(reference)
        {
          CHECK(order == *reference);
        }
        reference = order;
      }
    }
  }
}

TEST_CASE("a stamped write on a halted member lands exactly at its time during a peer's goal")
{
  for(auto const& how : variants())
  {
    CAPTURE(how.name);
    pinned const pin{how.one_cpu};
    rig          r{
        .members = {member("alu", "alu_entry"), member("idle", "idle_budget")}
    };
    r.build(how);
    auto       goal  = r.goal(0, {.budget = icount{4000}});
    auto const local = core::access(
        *r.members[1].on, 0, [](core::machine& m) { return m.virtual_clock().time().ps; });
    auto const               at = vtime{local + 333 * us_ps + 250'000};
    core::host_future<vtime> landed;
    {
      auto const held_host = r.members[1].on->lock();
      landed               = r.members[1].on->post(held_host, 0, [](core::machine&) {}, at);
    }
    REQUIRE(r.run({}).member == 0);
    CHECK(landed.get() == at);
  }
}

TEST_CASE("a stamped write on a sleeping goal member lands at its time, not at the barrier")
{
  for(auto const& how : {variants()[0], variants()[1], variants()[2]})
  {
    CAPTURE(how.name);
    rig r{
        .members = {member("alu", "alu_entry"), member("idle", "idle_forever")}
    };
    r.build(how);
    auto       peer  = r.goal(0, {.budget = icount{4000}});
    auto       sleep = r.goal(1, {.until = popcorn::address{0x0800'0001}});
    auto const local = core::access(
        *r.members[1].on, 0, [](core::machine& m) { return m.virtual_clock().time().ps; });
    auto const               at = vtime{local + 333 * us_ps + 250'000};
    core::host_future<vtime> landed;
    {
      auto const held_host = r.members[1].on->lock();
      landed               = r.members[1].on->post(held_host, 0, [](core::machine&) {}, at);
    }
    REQUIRE(r.run({}).member == 0);
    CHECK(landed.get() == at);
  }
}

TEST_CASE("a stamp overshot by a block past the barrier sees every event up to that block's end")
{
  std::optional<std::vector<std::uint64_t>> reference;
  for(auto const stamp_at : {std::int64_t{19 * us_ps}, std::int64_t{30 * us_ps}})
  {
    for(auto const latency : {20 * us_ps, 400 * us_ps})
    {
      for(auto const& how : {variants()[0], variants()[1]})
      {
        CAPTURE(stamp_at);
        CAPTURE(latency);
        CAPTURE(how.name);
        long_block made;
        rig        r{
            .members = {member("alu", "alu_entry"), long_block_member(made)}
        };
        r.build(how, latency);
        std::vector<std::uint64_t> seen;
        core::host_future<vtime>   landed;
        {
          auto const held_host = r.members[1].on->lock();
          landed               = r.members[1].on->post(
              held_host, 0,
              [&](core::machine&) {
                for(auto const& one : made.module->event_log)
                {
                  seen.push_back(one.payload);
                }
              },
              vtime{made.start.ps + static_cast<std::uint64_t>(stamp_at)});
        }
        REQUIRE(r.run(150 * us_ps).reason == core::pause_reason::system_time);
        CHECK(landed.get().ps >= made.start.ps + 100 * us_ps);
        CHECK(seen == std::vector<std::uint64_t>{1, 3});
        if(reference)
        {
          CHECK(seen == *reference);
        }
        reference = seen;
      }
    }
  }
}

TEST_CASE("a pause right after a held slice leaves a stamp to a plain run, after the held events")
{
  long_block made;
  rig        r{
      .members = {member("alu", "alu_entry"), long_block_member(made)}
  };
  r.build(variants()[0]);
  std::vector<std::uint64_t> seen;
  core::host_future<vtime>   landed;
  {
    auto const held_host = r.members[1].on->lock();
    landed               = r.members[1].on->post(
        held_host, 0,
        [&](core::machine& m) {
          MESSAGE(
              "body at ", m.virtual_clock().time().ps - made.start.ps, " pending ",
              m.pending_events(), " log ", made.module->event_log.size());
          for(auto const& one : made.module->event_log)
          {
            seen.push_back(one.payload);
          }
        },
        vtime{made.start.ps + 30 * us_ps});
  }
  REQUIRE(r.run(20 * us_ps).reason == core::pause_reason::system_time);
  CHECK_FALSE(landed.done());
  {
    auto const held = r.sys->lock();
    r.sys->finish(held);
  }
  r.sys.reset();
  core::host_future<core::goal_result> plain;
  {
    auto const held_host = r.members[1].on->lock();
    plain                = r.members[1].on->run(held_host, 0, {.budget = icount{1}});
  }
  std::ignore = plain.get();
  CHECK(landed.get().ps >= made.start.ps + 100 * us_ps);
  CHECK(seen == std::vector<std::uint64_t>{1, 3});
}

namespace
{
auto sleeper_with_event(std::uint64_t after) -> core::member_ref
{
  return member<popcorn::testing::test_arch>(
      "idle", "idle_forever", [after](popcorn::testing::machine_fixture<>& f) {
        auto const now = f.target->virtual_clock().now().value;
        std::ignore    = f.module->attached_events().schedule(icount{now + after}, logged_kind, 7);
      });
}
}

TEST_CASE("members asleep with nothing due pause as IDLE at the latest quiescent point")
{
  std::optional<std::vector<std::uint64_t>> reference;
  for(auto const& how : variants())
  {
    CAPTURE(how.name);
    pinned const pin{how.one_cpu};
    rig          r{
        .members = {sleeper_with_event(400), sleeper_with_event(9000)}
    };
    r.build(how);
    auto       a       = r.goal(0, {.until = popcorn::address{0x0800'0001}});
    auto       b       = r.goal(1, {.until = popcorn::address{0x0800'0001}});
    auto const stopped = r.run({});
    REQUIRE(stopped.reason == core::pause_reason::idle);
    CHECK(stopped.member == 1);
    CHECK(r.now_of(0) == stopped.at.ps);
    CHECK(r.now_of(1) == stopped.at.ps);
    CHECK(a.get().stopped.reason == stop_reason::idle);
    CHECK(b.get().stopped.reason == stop_reason::idle);
    CHECK(a.get().at == stopped.at);
    auto const start = core::access(
        *r.members[1].on, 0, [](core::machine& m) { return m.virtual_clock().time().ps; });
    CHECK(start == static_cast<std::uint64_t>(stopped.at.ps));
    {
      auto const held = r.sys->lock();
      CHECK(r.sys->undecided(held) == std::vector<std::size_t>{0, 1});
    }
    auto dump = r.dump();
    dump.push_back(static_cast<std::uint64_t>(stopped.at.ps));
    if(reference)
    {
      CHECK(dump == *reference);
    }
    reference = dump;
  }
}

TEST_CASE("a member waiting on a timer past the barrier is not idle")
{
  rig r{
      .members = {member("idle", "idle_entry"), sleeper_with_event(100)}
  };
  r.build(variants()[0]);
  REQUIRE(r.run(100 * us_ps).reason == core::pause_reason::system_time);
  auto                           timed = r.goal(0, {.until = popcorn::address{0x0800'0001}});
  auto                           other = r.goal(1, {.until = popcorn::address{0x0800'0001}});
  core::host_future<core::pause> pending;
  {
    auto const held = r.sys->lock();
    pending         = r.sys->resume(held);
  }
  std::this_thread::sleep_for(std::chrono::milliseconds{20});
  r.sys->request_pause();
  auto const stopped = pending.get();
  CHECK(stopped.reason == core::pause_reason::requested);
  CHECK(stopped.windows > 10);
  CHECK_FALSE(timed.done());
  CHECK_FALSE(other.done());
  auto const held = r.sys->lock();
  r.sys->withdraw(held, 0);
  CHECK_THROWS_AS(std::ignore = timed.get(), core::operation_cancelled);
  CHECK_THROWS_AS(r.sys->withdraw(held, 0), std::logic_error);
  CHECK(r.sys->undecided(held).empty());
}

TEST_CASE("an IDLE pause resolves goals that are not open and keeps the open ones")
{
  rig r{
      .members = {sleeper_with_event(400), sleeper_with_event(900)}
  };
  r.build(variants()[0]);
  auto       closed_goal = r.goal(0, {.until = popcorn::address{0x0800'0001}});
  auto       open        = r.goal(1, {.until = popcorn::address{0x0800'0001}, .open = true});
  auto const stopped     = r.run({});
  REQUIRE(stopped.reason == core::pause_reason::idle);
  CHECK(closed_goal.get().stopped.reason == stop_reason::idle);
  CHECK_FALSE(open.done());
  auto const held = r.sys->lock();
  CHECK(r.sys->undecided(held) == std::vector<std::size_t>{0});
}

TEST_CASE("an error in a halted member before a resolution found later in its window comes first")
{
  for(auto const& how : {variants()[0], variants()[1], variants()[2], variants()[4]})
  {
    CAPTURE(how.name);
    rig r{
        .members = {
                    member("alu", "alu_entry"),
                    member<throwing_arch>(
                "idle", "idle_forever", [](popcorn::testing::machine_fixture<throwing_arch>& f) {
                  auto const now = f.target->virtual_clock().now().value;
                  std::ignore    = f.module->attached_events().schedule(
                      icount{now + 50}, throwing_arch::throw_event, 0);
                })}
    };
    r.build(how);
    auto       goal    = r.goal(0, {.budget = icount{70}});
    auto const stopped = r.run({});
    REQUIRE(stopped.reason == core::pause_reason::error);
    CHECK(stopped.member == 1);
    CHECK(stopped.at.ps == 50 * 250'000);
    CHECK_THROWS_AS(std::ignore = goal.get(), test_failure);
  }
}

TEST_CASE("a goal cap stops the goal at its start plus the cap as a TIME stop")
{
  for(auto const& how : {variants()[0], variants()[1]})
  {
    CAPTURE(how.name);
    rig r{
        .members = {member("alu", "alu_entry"), member("idle", "idle_forever")}
    };
    r.build(how);
    auto capped =
        r.goal(1, {.until = popcorn::address{0x0800'0001}, .cap = core::sys_time{77 * us_ps}});
    auto const stopped = r.run({});
    REQUIRE(stopped.reason == core::pause_reason::delivered);
    auto const reached = capped.get();
    CHECK(reached.stopped.reason == stop_reason::time);
    CHECK(reached.stopped.detail == "goal cap");
    CHECK(reached.at.ps == 77 * us_ps);
  }
}

TEST_CASE("a reset request under the stop policy resolves the goal, reset_and_continue reboots")
{
  for(auto const policy :
      {popcorn::core::reset_request_policy::stop,
       popcorn::core::reset_request_policy::reset_and_continue})
  {
    CAPTURE(policy);
    auto resetting     = member("calls", "reset_now");
    resetting.on_reset = policy;
    rig r{
        .members = {member("alu", "alu_entry"), resetting}
    };
    r.build(variants()[0]);
    auto       goal    = r.goal(1, {.budget = icount{5000}});
    auto const stopped = r.run({});
    REQUIRE(stopped.reason == core::pause_reason::delivered);
    if(policy == popcorn::core::reset_request_policy::stop)
    {
      CHECK(goal.get().stopped.reason == stop_reason::reset_request);
    }
    else
    {
      CHECK(goal.get().stopped.reason == stop_reason::budget);
      CHECK(r.links[0]->receiver_resets.size() == 1);
    }
  }
}

TEST_CASE("close fails every pending goal with machine_closed")
{
  rig r{
      .members = {member("alu", "alu_entry"), member("alu", "alu_entry")}
  };
  r.build(variants()[0]);
  auto                           a = r.goal(0, {.budget = icount{1'000'000'000}});
  auto                           b = r.goal(1, {.budget = icount{1'000'000'000}});
  core::host_future<core::pause> pending;
  {
    auto const held = r.sys->lock();
    pending         = r.sys->resume(held);
  }
  std::this_thread::sleep_for(std::chrono::milliseconds{5});
  r.sys->close();
  CHECK(pending.get().reason == core::pause_reason::closed);
  CHECK_THROWS_AS(std::ignore = a.get(), core::machine_closed);
  CHECK_THROWS_AS(std::ignore = b.get(), core::machine_closed);
}

TEST_CASE("finish clears every hold and settles every link at the last pause point")
{
  rig r{
      .members = {member("alu", "alu_entry"), member("alu", "alu_entry")}
  };
  r.build(variants()[0]);
  auto       a       = r.goal(0, {.budget = icount{300}});
  auto const stopped = r.run({});
  REQUIRE(stopped.member == 0);
  r.release(0);
  auto const held = r.sys->lock();
  r.sys->finish(held);
  CHECK(r.links[0]->finished == std::vector{stopped.at.ps});
  for(auto const& one : r.members)
  {
    core::access(*one.on, 0, [](core::machine& m) { CHECK_FALSE(m.core_held()); });
  }
  CHECK_THROWS_AS(std::ignore = r.sys->post_goal(held, 0, {}), std::logic_error);
}

TEST_CASE("a link with something left to deliver keeps the system from IDLE")
{
  rig r{
      .members = {sleeper_with_event(400), sleeper_with_event(900)}
  };
  r.build(variants()[0]);
  r.links[0]->busy                 = true;
  auto                           a = r.goal(0, {.until = popcorn::address{0x0800'0001}});
  core::host_future<core::pause> pending;
  {
    auto const held = r.sys->lock();
    pending         = r.sys->resume(held);
  }
  std::this_thread::sleep_for(std::chrono::milliseconds{5});
  r.sys->request_pause();
  auto const refused = pending.get();
  CHECK(refused.reason == core::pause_reason::requested);
  CHECK(refused.windows > 0);
  CHECK_FALSE(a.done());
  r.links[0]->busy = false;
  CHECK(r.run({}).reason == core::pause_reason::idle);
  CHECK(a.get().stopped.reason == stop_reason::idle);
}

TEST_CASE("two stops in one window and in two windows give the same pause sequence")
{
  auto const counting = [](std::uint32_t loops) {
    return member<popcorn::testing::test_arch>(
        "branches", "branches",
        [loops](popcorn::testing::machine_fixture<>& f) { f.set("r0", loops); });
  };
  std::optional<std::vector<std::pair<int, std::int64_t>>> reference;
  for(auto const& how : {variants()[0], variants()[1], variants()[2], variants()[4]})
  {
    CAPTURE(how.name);
    rig r{
        .members = {counting(2), counting(10)}
    };
    r.links.push_back(std::make_shared<fake_link>(0, 1, 20 * us_ps));
    if(how.lowered != 0)
    {
      r.links.push_back(std::make_shared<fake_link>(1, 0, 20 * us_ps / how.lowered));
    }
    std::vector<std::shared_ptr<core::member_link>> wires{r.links.begin(), r.links.end()};
    r.sys = std::make_unique<core::system>(
        r.members, std::move(wires), core::default_wait_policy(2), how.bounds);
    {
      auto const                        held = r.sys->lock();
      std::vector<core::sys_time> const offsets(2);
      r.sys->start(held, offsets);
    }
    std::vector<std::pair<int, std::int64_t>> order;
    for(int round = 0; round < 5; ++round)
    {
      auto const stopped = r.run(200 * us_ps);
      order.emplace_back(static_cast<int>(stopped.reason), stopped.at.ps);
      if(stopped.member)
      {
        CHECK(r.now_of(*stopped.member) == stopped.at.ps);
      }
      if(stopped.reason != core::pause_reason::delivered)
      {
        break;
      }
      r.release(*stopped.member);
      CHECK(stopped.stopped.reason == stop_reason::user);
    }
    REQUIRE(order.size() == 3);
    if(reference)
    {
      CHECK(order == *reference);
    }
    reference = order;
  }
}

TEST_CASE("a resume that cannot start a member parks the members it started")
{
  rig r{.members = sleeper_and_runner()};
  r.build(variants()[0]);
  r.members[1].on->close();
  {
    auto const held = r.sys->lock();
    CHECK_THROWS_AS(std::ignore = r.sys->resume(held, core::sys_time{ms_ps}), core::machine_closed);
  }
  while(not parked(r.members[0]))
  {
    std::this_thread::yield();
  }
  auto const held = r.sys->lock();
  CHECK_THROWS_AS(std::ignore = r.sys->resume(held), core::machine_closed);
}

TEST_CASE("a halted member whose next event lies past the barrier runs no slice per window")
{
  rig r{
      .members = {
                  member<popcorn::testing::test_arch>(
              "idle", "idle_forever",
                  [](popcorn::testing::machine_fixture<>& f) {
                auto const now = f.target->virtual_clock().now().value;
                std::ignore =
                    f.module->attached_events().schedule(icount{now + 1'000'000'000}, 1, 7);
              }),
                  member("alu", "alu_entry")}
  };
  r.build(variants()[0]);
  std::int64_t windows = 0;
  std::int64_t slices  = 0;
  std::int64_t pauses  = 0;
  while(windows < 100)
  {
    core::host_future<core::pause> pending;
    {
      auto const held = r.sys->lock();
      pending         = r.sys->resume(held);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
    r.sys->request_pause();
    auto const stopped = pending.get();
    REQUIRE(stopped.reason == core::pause_reason::requested);
    windows += static_cast<std::int64_t>(stopped.windows);
    slices += static_cast<std::int64_t>(stopped.slices);
    ++pauses;
  }
  CHECK((slices - 4 * pauses) * 10 < windows);
}

TEST_CASE("a TIME run with every member released pauses at T or past it, naming no member")
{
  rig r{
      .members = {member("alu", "alu_entry"), member("alu", "alu_entry")}
  };
  r.build(variants()[0]);
  for(std::size_t index = 0; index < 2; ++index)
  {
    auto goal = r.goal(index, {.budget = icount{100 + 50 * index}});
    REQUIRE(r.run({}).member == index);
    r.release(index);
  }
  auto const furthest = std::max(r.now_of(0), r.now_of(1));
  auto const behind   = r.run(us_ps);
  CHECK(behind.reason == core::pause_reason::system_time);
  CHECK_FALSE(behind.member.has_value());
  CHECK(behind.at.ps == furthest);
  auto const ahead = r.run(500 * us_ps);
  CHECK(ahead.reason == core::pause_reason::system_time);
  CHECK_FALSE(ahead.member.has_value());
  CHECK(ahead.at.ps == 500 * us_ps);
  CHECK(r.now_of(0) == 500 * us_ps);
  CHECK(r.now_of(1) == 500 * us_ps);
}

TEST_CASE("a tie found rounds apart is delivered by member index at any quantum")
{
  for(auto const& how : {variants()[0], variants()[1], variants()[2]})
  {
    CAPTURE(how.name);
    rig r{
        .members = {
                    member("idle", "idle_forever"), member("idle", "idle_forever"),
                    member("alu", "alu_entry")}
    };
    r.links.push_back(std::make_shared<fake_link>(0, 1, 20 * us_ps));
    if(how.lowered != 0)
    {
      r.links.push_back(std::make_shared<fake_link>(1, 0, 20 * us_ps / how.lowered));
    }
    std::vector<std::shared_ptr<core::member_link>> wires{r.links.begin(), r.links.end()};
    r.sys = std::make_unique<core::system>(
        r.members, std::move(wires), core::default_wait_policy(3), how.bounds);
    {
      auto const                        held = r.sys->lock();
      std::vector<core::sys_time> const offsets(3);
      r.sys->start(held, offsets);
    }
    constexpr std::int64_t tie    = 15 * us_ps;
    auto                   first  = r.goal(0, {.until_time = core::sys_time{5 * us_ps}});
    auto                   other  = r.goal(1, {.until_time = core::sys_time{tie}});
    auto                   middle = r.goal(2, {.budget = icount{40}});
    std::vector<std::pair<std::size_t, std::int64_t>> order;
    auto const                                        f = r.run({});
    order.emplace_back(*f.member, f.at.ps);
    auto       again = r.goal(0, {.until_time = core::sys_time{tie}});
    auto const h     = r.run({});
    order.emplace_back(*h.member, h.at.ps);
    r.release(2);
    auto const t0 = r.run({});
    order.emplace_back(*t0.member, t0.at.ps);
    r.release(*t0.member);
    auto const t1 = r.run({});
    order.emplace_back(*t1.member, t1.at.ps);
    CHECK(
        order
        == std::vector<std::pair<std::size_t, std::int64_t>>{
            {0, 5 * us_ps},
            {2, h.at.ps  },
            {0, tie      },
            {1, tie      }
    });
    CHECK(h.at.ps > 5 * us_ps);
    CHECK(h.at.ps < tie);
  }
}

TEST_CASE("finish fails a goal still pending with operation_cancelled")
{
  rig r{
      .members = {member("alu", "alu_entry"), member("idle", "idle_forever")}
  };
  r.build(variants()[0]);
  auto       a       = r.goal(0, {.budget = icount{300}});
  auto       pending = r.goal(1, {.until = popcorn::address{0x0800'0001}});
  auto const stopped = r.run({});
  REQUIRE(stopped.member == 0);
  r.release(0);
  {
    auto const held = r.sys->lock();
    r.sys->finish(held);
  }
  REQUIRE(pending.done());
  CHECK_THROWS_AS(std::ignore = pending.get(), core::operation_cancelled);
}

namespace
{
auto tracked_sleeper(std::uint64_t after, popcorn::testing::test_arch*& module) -> core::member_ref
{
  return member<popcorn::testing::test_arch>(
      "idle", "idle_forever", [after, &module](popcorn::testing::machine_fixture<>& f) {
        auto const now = f.target->virtual_clock().now().value;
        std::ignore    = f.module->attached_events().schedule(icount{now + after}, logged_kind, 7);
        module         = f.module;
      });
}

auto both_asleep(rig const& r, core::host_future<core::pause> const& pending) -> bool
{
  for(;;)
  {
    if(pending.done())
    {
      return false;
    }
    if(std::ranges::all_of(r.members, [](core::member_ref const& one) {
         return one.on->state(0) == core::host_state::idle_wait;
       }))
    {
      return true;
    }
    std::this_thread::yield();
  }
}
}

TEST_CASE("open goals asleep on IDLE wait until a stamp wakes one member, then all resume")
{
  std::optional<std::vector<std::uint64_t>> reference;
  for(auto const& how : {variants()[0], variants()[1], variants()[2], variants()[6]})
  {
    CAPTURE(how.name);
    pinned const                 pin{how.one_cpu};
    popcorn::testing::test_arch* module = nullptr;
    rig                          r{
        .members = {sleeper_with_event(400), tracked_sleeper(900, module)}
    };
    r.build(how);
    auto       a     = r.goal(0, {.until = popcorn::address{0x0800'0001}, .open = true});
    auto       b     = r.goal(1, {.until = popcorn::address{0x0800'0001}, .open = true});
    auto const start = core::access(
        *r.members[1].on, 0, [](core::machine& m) { return m.virtual_clock().now().value; });
    auto const local = core::access(
        *r.members[1].on, 0, [](core::machine& m) { return m.virtual_clock().time().ps; });
    core::host_future<core::pause> pending;
    {
      auto const held = r.sys->lock();
      pending         = r.sys->resume(held);
    }
    REQUIRE(both_asleep(r, pending));
    core::host_future<vtime> landed;
    {
      auto const held_host = r.members[1].on->lock();
      landed               = r.members[1].on->post(
          held_host, 0,
          [module](core::machine& m) {
            std::ignore = module->attached_events().schedule(
                icount{m.virtual_clock().now().value + 100}, logged_kind, 8);
          },
          vtime{local + 900 * 250'000 + 50 * us_ps});
    }
    std::ignore = landed.get();
    REQUIRE(both_asleep(r, pending));
    r.sys->request_pause();
    auto const stopped = pending.get();
    CHECK(stopped.reason == core::pause_reason::requested);
    CHECK_FALSE(a.done());
    CHECK_FALSE(b.done());
    auto const logged = core::access(*r.members[1].on, 0, [&](core::machine&) {
      return module->event_log.back().at.value - start;
    });
    CHECK(logged == 900 + 200 + 100);
    auto dump = r.dump();
    dump.push_back(landed.get().ps);
    dump.push_back(logged);
    if(reference)
    {
      CHECK(dump == *reference);
    }
    reference = dump;
  }
}

TEST_CASE("close and wake reach a system asleep on IDLE inside open goals")
{
  rig r{
      .members = {sleeper_with_event(400), sleeper_with_event(900)}
  };
  r.build(variants()[0]);
  auto a = r.goal(0, {.until = popcorn::address{0x0800'0001}, .open = true});
  auto b = r.goal(1, {.until = popcorn::address{0x0800'0001}, .open = true});
  core::host_future<core::pause> pending;
  {
    auto const held = r.sys->lock();
    pending         = r.sys->resume(held);
  }
  REQUIRE(both_asleep(r, pending));
  r.sys->wake();
  REQUIRE(both_asleep(r, pending));
  r.sys->close();
  CHECK(pending.get().reason == core::pause_reason::closed);
  CHECK_THROWS_AS(std::ignore = a.get(), core::machine_closed);
  CHECK(parked(r.members[0]));
  CHECK(parked(r.members[1]));
}

TEST_CASE("a wake that lands between the sleep decision and the host's wait is not lost")
{
  auto const slices = [](bool hooked) {
    rig r{
        .members = {sleeper_with_event(400), sleeper_with_event(900)}
    };
    r.links.push_back(std::make_shared<fake_link>(0, 1, 20 * us_ps));
    std::atomic<bool>                woken{false};
    core::system*                    owner = nullptr;
    std::function<void(std::size_t)> hook;
    if(hooked)
    {
      hook = [&woken, &owner](std::size_t) {
        if(not woken.exchange(true))
        {
          owner->wake();
        }
      };
    }
    r.sys = std::make_unique<core::system>(
        r.members, std::vector<std::shared_ptr<core::member_link>>{r.links[0]},
        core::default_wait_policy(2), core::slice_bounds{}, std::function<void(std::size_t)>{},
        hook);
    owner = r.sys.get();
    {
      auto const                  held = r.sys->lock();
      std::vector<core::sys_time> offsets(2);
      r.sys->start(held, offsets);
    }
    auto a = r.goal(0, {.until = popcorn::address{0x0800'0001}, .open = true});
    auto b = r.goal(1, {.until = popcorn::address{0x0800'0001}, .open = true});
    core::host_future<core::pause> pending;
    {
      auto const held = r.sys->lock();
      pending         = r.sys->resume(held);
    }
    REQUIRE(both_asleep(r, pending));
    r.sys->request_pause();
    auto const stopped = pending.get();
    REQUIRE(stopped.reason == core::pause_reason::requested);
    return stopped.slices;
  };
  auto const quiet = slices(false);
  CHECK(slices(true) > quiet);
}
