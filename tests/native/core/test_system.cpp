#include <popcorn/arch/arch.hpp>
#include <popcorn/core/errors.hpp>
#include <popcorn/core/host.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/core/member_link.hpp>
#include <popcorn/core/snapshot.hpp>
#include <popcorn/core/system.hpp>
#include <popcorn/core/window_barrier.hpp>
#include <popcorn/periph/model.hpp>

#include "../../../native/src/core/promise_record.hpp"
#include "../../../native/src/core/system_seam.hpp"
#include "../support/machine_fixture.hpp"
#include "../support/test_arch.hpp"

#include <doctest/doctest.h>
#include <reflex/serde/bson.hpp>

#include <sched.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <latch>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <span>
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

constexpr std::int64_t us_ps          = 1'000'000;
constexpr std::int64_t instruction_ps = 250'000;
constexpr std::int64_t ms_ps          = 1'000'000'000;

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
  std::int64_t barrier{};
  std::int64_t next{};

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

  void enter_slice(core::time_map) override
  {
    entered.fetch_add(1, std::memory_order_release);
  }

  auto posted_count() const noexcept -> std::uint64_t override
  {
    return entered.load(std::memory_order_acquire);
  }

  void sender_reset(core::sys_time at) override
  {
    sender_resets.push_back(at.ps);
  }

  void receiver_reset(core::sys_time at) override
  {
    receiver_resets.push_back(at.ps);
  }

  auto drain(
      std::uint64_t         through,
      core::sys_time const& barrier,
      core::sys_time const& next,
      core::time_map,
      core::sys_time const& shown) -> std::optional<core::link_fault> override
  {
    if(on_shown)
    {
      on_shown(barrier.ps, next.ps, shown.ps);
    }
    CHECK(through >= last_through);
    last_through = through;
    if(on_through)
    {
      on_through(through);
    }
    if(drains.size() < kept_drains)
    {
      drains.push_back({.barrier = barrier.ps, .next = next.ps});
    }
    if(on_drain)
    {
      on_drain(barrier.ps);
    }
    if(due_at and *due_at < barrier.ps)
    {
      due_at.reset();
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

  auto due() const -> std::optional<core::sys_time> override
  {
    if(due_at)
    {
      return core::sys_time{*due_at};
    }
    return std::nullopt;
  }

  auto quiet() const -> bool override
  {
    return not busy;
  }

  void release_through(core::sys_time at) override
  {
    if(on_release)
    {
      on_release(at.ps);
    }
  }

  void finish(core::sys_time at) override
  {
    finished.push_back(at.ps);
  }

  void save(std::vector<std::byte>& out) const override
  {
    out.push_back(state);
  }

  void load(std::span<std::byte const> in) override
  {
    if(fail_load or in.size() != 1)
    {
      throw popcorn::config_error("fake link", 0, "bytes", "one state byte");
    }
    state = in[0];
  }

  std::atomic<std::uint64_t>                                              entered{};
  std::uint64_t                                                           last_through{};
  std::vector<drained>                                                    drains;
  std::vector<std::int64_t>                                               sender_resets;
  std::vector<std::int64_t>                                               receiver_resets;
  std::optional<std::int64_t>                                             fault_at{};
  std::optional<std::int64_t>                                             due_at{};
  std::move_only_function<void(std::int64_t)>                             on_drain{};
  std::move_only_function<void(std::int64_t, std::int64_t, std::int64_t)> on_shown{};
  std::move_only_function<void(std::int64_t)>                             on_release{};
  std::move_only_function<void(std::uint64_t)>                            on_through{};
  bool                                                                    busy{};
  std::byte                                                               state{};
  bool                                                                    fail_load{};
  std::vector<std::int64_t>                                               finished{};
  bool                                                                    reported{};

private:
  std::size_t  from_;
  std::size_t  to_;
  std::int64_t latency_;
};

template <typename Arch = popcorn::testing::test_arch>
auto member(
    std::string_view                                                  firmware,
    std::string_view                                                  entry,
    std::function_ref<void(popcorn::testing::machine_fixture<Arch>&)> prepare =
        [](popcorn::testing::machine_fixture<Arch>&) {}) -> core::member_ref
{
  popcorn::testing::machine_fixture<Arch> f{m33(), firmware, core::machine_options{}};
  f.set("pc", sym(firmware, entry));
  f.set("r0", 0);
  prepare(f);
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
  core::sync_mode              mode{core::sync_mode::barrier};
  std::int64_t                 progress{core::system::default_progress.ps};
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
      {.name = "clocks", .mode = core::sync_mode::clocks},
      {.name    = "clocks 997 q=l/3",
       .lowered = 3,
       .bounds  = {.max_instructions = icount{997}, .wall_budget = std::chrono::microseconds{10}},
       .mode    = core::sync_mode::clocks},
      {.name = "clocks sleeps 1", .seed = 1, .mode = core::sync_mode::clocks},
      {.name = "clocks one cpu", .one_cpu = true, .mode = core::sync_mode::clocks},
      {.name = "clocks k=25us", .mode = core::sync_mode::clocks, .progress = 25 * us_ps},
      {.name     = "clocks k=200us 997",
       .bounds   = {.max_instructions = icount{997}, .wall_budget = std::chrono::microseconds{10}},
       .mode     = core::sync_mode::clocks,
       .progress = 200 * us_ps},
      {.name = "clocks no progress", .mode = core::sync_mode::clocks, .progress = 0},
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
  std::vector<core::member_ref>              members;
  std::vector<std::shared_ptr<fake_link>>    links{};
  std::unique_ptr<core::system>              sys{};
  std::move_only_function<void(std::size_t)> arrival{};
  std::move_only_function<void(std::size_t)> slept{};

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
    std::move_only_function<void(std::size_t)> hook;
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
    if(not hook and arrival)
    {
      hook = std::move(arrival);
    }
    sys = std::make_unique<core::system>(
        members, std::move(wires), how.bounds, std::move(hook), std::move(slept), how.mode,
        core::system::default_lookahead, core::sys_time{how.progress});
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

  void release(std::size_t member, bool halt = true)
  {
    auto const held = sys->lock();
    sys->release(held, member, halt);
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
  for(auto const& how : {variants()[0], variants()[3], variants()[7], variants()[8]})
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
              .barrier = static_cast<std::int64_t>(window + 1) * q,
              .next    = static_cast<std::int64_t>(window + 2) * q});
    }
    auto const& confirm = link->drains.back();
    CHECK(confirm.barrier == 1 * ms_ps);
    CHECK(confirm.next == 1 * ms_ps);
  }
  auto const held = r.sys->lock();
  CHECK(r.sys->pending_barrier(held).ps == static_cast<std::int64_t>(stopped.windows + 1) * q);
}

TEST_CASE("a drain takes the posts its sender made before the barrier, not those after it")
{
  rig        r{.members = sleeper_and_runner()};
  auto const slow = std::make_shared<fake_link>(1, 0, 20 * us_ps);
  auto const fast = std::make_shared<fake_link>(1, 0, 20 * us_ps);
  slow->on_drain  = [](std::int64_t) {
    std::this_thread::sleep_for(std::chrono::microseconds{200});
  };
  std::vector<std::uint64_t> arrived;
  std::vector<std::uint64_t> drained;
  fast->on_through = [&drained](std::uint64_t through) {
    if(drained.empty() or drained.back() != through)
    {
      drained.push_back(through);
    }
  };
  r.sys = std::make_unique<core::system>(
      r.members, std::vector<std::shared_ptr<core::member_link>>{slow, fast},
      core::slice_bounds{
          .max_instructions = icount{997}, .wall_budget = std::chrono::microseconds{10}},
      [&arrived, &fast](std::size_t index) {
        auto const count = fast->posted_count();
        if(index == 1 and (arrived.empty() or arrived.back() != count))
        {
          arrived.push_back(count);
        }
      },
      std::move_only_function<void(std::size_t)>{}, core::sync_mode::barrier);
  {
    auto const                        held = r.sys->lock();
    std::vector<core::sys_time> const offsets(2);
    r.sys->start(held, offsets);
  }
  auto const endless = r.goal(1, {.budget = icount{1'000'000'000'000}});
  auto const stopped = r.run(1 * ms_ps);
  REQUIRE(stopped.reason == core::pause_reason::system_time);
  REQUIRE(stopped.windows > 20);
  REQUIRE(drained.size() >= stopped.windows);
  REQUIRE(arrived.size() >= drained.size());
  arrived.resize(drained.size());
  CHECK(drained == arrived);
}

TEST_CASE("a member linked to itself drains its own wire each window, at its barrier")
{
  rig r{.members = sleeper_and_runner()};
  r.links.push_back(std::make_shared<fake_link>(1, 1, 20 * us_ps));
  r.links.push_back(std::make_shared<fake_link>(0, 1, 50 * us_ps));
  r.sys = std::make_unique<core::system>(
      r.members, std::vector<std::shared_ptr<core::member_link>>{r.links[0], r.links[1]},
      core::slice_bounds{}, std::move_only_function<void(std::size_t)>{},
      std::move_only_function<void(std::size_t)>{}, core::sync_mode::barrier);
  {
    auto const                        held = r.sys->lock();
    std::vector<core::sys_time> const offsets(2);
    r.sys->start(held, offsets);
  }
  auto const stopped = r.run(1 * ms_ps);
  REQUIRE(stopped.reason == core::pause_reason::system_time);
  auto const q = r.sys->quantum().ps;
  CHECK(q == 20 * us_ps);
  for(auto const& link : r.links)
  {
    CAPTURE(link->sender());
    REQUIRE(link->drains.size() == stopped.windows + 1);
    for(std::uint64_t window = 0; window < stopped.windows; ++window)
    {
      CHECK(
          link->drains[window]
          == drained{
              .barrier = static_cast<std::int64_t>(window + 1) * q,
              .next    = static_cast<std::int64_t>(window + 2) * q});
    }
    CHECK(link->drains.back().barrier == 1 * ms_ps);
  }
}

TEST_CASE("offsets set each member's system time and base, and a reset while paused keeps the time")
{
  rig r{.members = sleeper_and_runner()};
  r.links.push_back(std::make_shared<fake_link>(0, 1, 20 * us_ps));
  r.sys = std::make_unique<core::system>(
      r.members, std::vector<std::shared_ptr<core::member_link>>{r.links[0]});
  {
    auto const                        held = r.sys->lock();
    std::vector<core::sys_time> const offsets{core::sys_time{0}, core::sys_time{37 * us_ps}};
    r.sys->start(held, offsets);
    CHECK(r.sys->time_of(held, 1).ps == 37 * us_ps);
    CHECK(r.sys->base_of(1).ps == 37 * us_ps);
  }
  REQUIRE(r.run(500 * us_ps).reason == core::pause_reason::system_time);
  auto const held   = r.sys->lock();
  auto const before = r.sys->time_of(held, 0);
  CHECK(r.sys->base_of(0).ps == 0);
  r.sys->reset(held, 0, popcorn::reset_kind::software);
  CHECK(r.sys->time_of(held, 0) == before);
  CHECK(r.sys->base_of(0) == before);
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

namespace
{
class at_scope_end
{
public:
  explicit at_scope_end(std::move_only_function<void()> run) : run_{std::move(run)}
  {}
  at_scope_end(at_scope_end const&)                    = delete;
  auto operator=(at_scope_end const&) -> at_scope_end& = delete;

  ~at_scope_end()
  {
    run_();
  }

private:
  std::move_only_function<void()> run_;
};

void open_gate(std::atomic<bool>& gate) noexcept
{
  gate.store(true);
  gate.notify_all();
}
}

TEST_CASE("a pause requested right after resume ends that flight")
{
  auto const asked = std::make_shared<std::atomic<bool>>(false);
  rig        r{
      .members = {member("alu", "alu_entry"), member("alu", "alu_entry")},
      .arrival = [asked](std::size_t) { asked->wait(false); }
  };
  r.build(variants()[0]);
  at_scope_end const             release{[asked] { open_gate(*asked); }};
  core::host_future<core::pause> pending;
  {
    auto const held = r.sys->lock();
    pending         = r.sys->resume(held, core::sys_time{10 * ms_ps});
  }
  r.sys->request_pause();
  open_gate(*asked);
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
  auto const asked = std::make_shared<std::atomic<bool>>(false);
  rig r{.members = sleeper_and_runner(), .arrival = [asked](std::size_t) { asked->wait(false); }};
  r.build(variants()[0]);
  at_scope_end const release{[asked] { open_gate(*asked); }};
  CHECK(r.sys->hosts() == std::vector{r.members[0].on, r.members[1].on});
  core::host_future<core::pause> pending;
  {
    auto const held = r.sys->lock();
    pending         = r.sys->resume(held, core::sys_time{10'000 * ms_ps});
    CHECK_THROWS_AS(std::ignore = r.sys->time_of(held, 0), popcorn::machine_running);
    CHECK_THROWS_AS(std::ignore = r.sys->pending_barrier(held), popcorn::machine_running);
  }
  CHECK(r.sys->base_of(0).ps == 0);
  CHECK(r.sys->base_of(1).ps == 0);
  r.sys->request_pause();
  open_gate(*asked);
  auto const stopped = pending.get();
  CHECK(stopped.reason == core::pause_reason::requested);
  auto const held = r.sys->lock();
  CHECK(r.sys->time_of(held, 0) == stopped.at);
  CHECK(r.sys->pending_barrier(held) == stopped.at);
}

TEST_CASE("work timing counts every barrier round while measured and stays zero otherwise")
{
  rig r{.members = sleeper_and_runner()};
  r.build(variants()[0]);
  auto const plain = r.run(200 * us_ps);
  REQUIRE(plain.reason == core::pause_reason::system_time);
  CHECK(plain.work.rounds == 0);
  CHECK(plain.work.serial.count() == 0);
  CHECK(plain.work.drains.count() == 0);
  {
    auto const held = r.sys->lock();
    r.sys->measure_work(held, true);
  }
  auto const timed = r.run(1 * ms_ps);
  REQUIRE(timed.reason == core::pause_reason::system_time);
  CHECK(timed.phase_b == 0);
  CHECK(timed.work.rounds == timed.windows + 2);
  CHECK(timed.work.ideal.count() > 0);
  CHECK(timed.work.serial > timed.work.ideal);
  CHECK(timed.work.serial <= 2 * timed.work.ideal);
  CHECK(timed.work.drains.count() > 0);
  CHECK(timed.work.drains < timed.work.serial);

  std::uint64_t phase_b = 0;
  for(int attempt = 0; attempt < 1000 and phase_b == 0; ++attempt)
  {
    core::host_future<core::pause> pending;
    {
      auto const held = r.sys->lock();
      pending         = r.sys->resume(held);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
    r.sys->request_pause();
    auto const halted = pending.get();
    REQUIRE(halted.reason == core::pause_reason::requested);
    CHECK(halted.work.rounds == halted.windows + halted.phase_b + 2);
    phase_b = halted.phase_b;
  }
  CHECK(phase_b > 0);
  {
    auto const held = r.sys->lock();
    r.sys->measure_work(held, false);
  }
  auto const after = r.run(2 * ms_ps);
  REQUIRE(after.reason == core::pause_reason::system_time);
  CHECK(after.work.rounds == 0);
  CHECK(after.work.ideal.count() == 0);
  CHECK(after.work.drains.count() == 0);
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
        r.members, std::vector<std::shared_ptr<core::member_link>>{r.links[0]});
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
    for(auto const& how :
        {variants()[0], variants()[1], variants()[2], variants()[7], variants()[8]})
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
  for(auto const& how : {variants()[0], variants()[1], variants()[4], variants()[7], variants()[8]})
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
        r.members, std::vector<std::shared_ptr<core::member_link>>{r.links[0]}, how.bounds);
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

TEST_CASE("a system that fails to link a member leaves the members it linked free")
{
  rig r{
      .members = {member("alu", "alu_entry"), member("alu", "alu_entry")}
  };
  core::host_future<core::goal_result> busy;
  {
    auto const held_host = r.members[1].on->lock();
    busy = r.members[1].on->run(held_host, 0, core::run_goal{.budget = icount{1'000'000'000'000}});
  }
  CHECK_THROWS_AS(r.build(variants()[0]), popcorn::machine_busy);
  r.members[1].on->cancel(0);
  CHECK_THROWS_AS(std::ignore = busy.get(), core::operation_cancelled);
  core::host_future<core::goal_result> own;
  {
    auto const held_host = r.members[0].on->lock();
    own                  = r.members[0].on->run(held_host, 0, core::run_goal{.budget = icount{10}});
  }
  CHECK(own.get().stopped.reason == stop_reason::budget);
}

TEST_CASE("a system with no arrival hook runs the same windows as one with a counting hook")
{
  std::atomic<std::uint64_t> arrivals{0};
  rig                        plain{.members = sleeper_and_runner()};
  rig                        counted{
      .members = sleeper_and_runner(),
      .arrival = [&arrivals](std::size_t) { arrivals.fetch_add(1); },
  };
  plain.build(variants()[0]);
  counted.build(variants()[0]);
  auto const bare   = plain.run(1 * ms_ps);
  auto const hooked = counted.run(1 * ms_ps);
  CHECK(bare.reason == hooked.reason);
  CHECK(bare.at == hooked.at);
  CHECK(bare.windows == hooked.windows);
  CHECK(bare.windows > 0);
  CHECK(arrivals.load() >= 2 * hooked.windows);
}

auto holding_rig(std::atomic<bool>& held) -> rig
{
  return rig{
      .members = {member("alu", "alu_entry"), member("alu", "alu_entry")},
      .arrival =
          [&held](std::size_t index) {
            if(index == 1)
            {
              held.store(true);
              held.notify_all();
            }
                  },
  };
}

auto close_holding(rig& r, std::atomic<bool> const& held) -> core::host_future<core::goal_reached>
{
  r.build(variants()[0]);
  auto                           endless = r.goal(0, {.budget = icount{1'000'000'000'000}});
  core::host_future<core::pause> pending;
  {
    auto const held_system = r.sys->lock();
    pending                = r.sys->resume(held_system, std::nullopt);
  }
  held.wait(false);
  r.sys->close();
  REQUIRE(pending.get().reason == core::pause_reason::closed);
  return endless;
}

TEST_CASE("a closed system releases its members before its closed pause is out")
{
  std::atomic<bool>                    held{};
  auto                                 r       = holding_rig(held);
  auto const                           endless = close_holding(r, held);
  core::host_future<core::goal_result> own;
  {
    auto const held_host = r.members[1].on->lock();
    own                  = r.members[1].on->run(held_host, 0, core::run_goal{.budget = icount{10}});
  }
  CHECK(own.get().stopped.reason == stop_reason::budget);
  core::access(*r.members[1].on, 0, [](core::machine& m) { CHECK_FALSE(m.core_held()); });
}

TEST_CASE("a member its closed system held takes a call before any run")
{
  std::atomic<bool>               held{};
  auto                            r         = holding_rig(held);
  auto const                      endless   = close_holding(r, held);
  auto&                           on        = *r.members[1].on;
  auto const                      held_host = on.lock();
  core::owner_scope const         owning{on, held_host};
  auto&                           target = on.member(held_host, 0);
  popcorn::arch::call_frame const frame{
      .entry           = sym("alu", "alu_entry"),
      .returns         = popcorn::arch::value_kind::i32,
      .abi             = popcorn::arch::float_abi::soft,
      .return_sentinel = 0x3000'0000};
  REQUIRE(target.core_held());
  CHECK_THROWS_AS(target.begin_call(frame), popcorn::machine_busy);
  on.give_back_core(held_host, 0);
  CHECK_FALSE(target.core_held());
  target.begin_call(frame);
  CHECK(target.in_call());
  std::ignore = target.end_call();
  CHECK_FALSE(target.core_held());
}

TEST_CASE("a lone goal posted while its closed system is destroyed reads no link unlocked")
{
  std::atomic<bool>                    held{};
  auto                                 r       = holding_rig(held);
  auto const                           endless = close_holding(r, held);
  auto&                                on      = *r.members[1].on;
  std::atomic<bool>                    inside{};
  std::atomic<bool>                    destroyed{};
  core::host_future<core::goal_result> own;
  std::jthread                         poster{[&] {
    auto const held_host = on.lock();
    own                  = on.run(
        held_host, 0, core::run_goal{.budget = icount{1'000'000'000'000}, .stop_when = [&] {
                                       if(not inside.exchange(true, std::memory_order_relaxed))
                                       {
                                         while(not destroyed.load(std::memory_order_relaxed))
                                         {
                                           std::this_thread::yield();
                                         }
                                       }
                                       return false;
                                     }});
  }};
  while(not inside.load(std::memory_order_relaxed))
  {
    std::this_thread::yield();
  }
  r.sys.reset();
  destroyed.store(true, std::memory_order_relaxed);
  poster.join();
  on.cancel(0);
  CHECK_THROWS_AS(std::ignore = own.get(), core::operation_cancelled);
  core::access(on, 0, [](core::machine& m) { CHECK_FALSE(m.core_held()); });
}

TEST_CASE("a member goal arriving at its until address stops there with nothing executed")
{
  for(auto const& how : {variants()[0], variants()[1], variants()[7], variants()[8]})
  {
    for(auto const arrive : {false, true})
    {
      CAPTURE(how.name);
      CAPTURE(arrive);
      long_block made;
      rig        r{
          .members = {member("alu", "alu_entry"), long_block_member(made)}
      };
      r.build(how);
      core::member_goal wanted{.until = scratch_code, .until_time = core::sys_time{50 * us_ps}};
      if(arrive)
      {
        wanted.arrive_at = {scratch_code};
      }
      auto goal = r.goal(1, std::move(wanted));
      REQUIRE(r.run({}).member == 1);
      auto const reached  = goal.get();
      auto const executed = [&] {
        auto const held = r.sys->lock();
        return r.sys->executed_of(held, 1).value;
      }();
      if(arrive)
      {
        CHECK(reached.stopped.reason == stop_reason::address);
        CHECK(executed == 0);
      }
      else
      {
        CHECK(reached.stopped.reason == stop_reason::time);
        CHECK(executed > 0);
      }
    }
  }
}

TEST_CASE("a member goal with several until addresses stops at the first one reached")
{
  constexpr popcorn::address near  = scratch_code + 2 * 100;
  constexpr popcorn::address far   = scratch_code + 2 * 300;
  auto const                 pc_of = [](rig& r) {
    return core::access(*r.members[1].on, 0, [](core::machine& m) {
      return static_cast<popcorn::address>(m.regs().read(*m.regs().find("pc")));
    });
  };
  for(auto const& how : {variants()[0], variants()[1], variants()[7], variants()[8]})
  {
    CAPTURE(how.name);
    long_block made;
    rig        r{
        .members = {member("alu", "alu_entry"), long_block_member(made)}
    };
    r.build(how);
    auto const both = [&] {
      return core::member_goal{
          .until = far, .until_also = {near}, .until_time = core::sys_time{1 * ms_ps}};
    };
    auto first = r.goal(1, both());
    REQUIRE(r.run({}).member == 1);
    CHECK(first.get().stopped.reason == stop_reason::address);
    CHECK(pc_of(r) == near);
    auto second = r.goal(1, both());
    REQUIRE(r.run({}).member == 1);
    CHECK(second.get().stopped.reason == stop_reason::address);
    CHECK(pc_of(r) == far);
    auto const held = r.sys->lock();
    CHECK(r.sys->executed_of(held, 1).value == 300);
  }
}

TEST_CASE("a goal posted to a halted member while paused starts at the delivered point")
{
  for(auto const& how : {variants()[0], variants()[1], variants()[7], variants()[8]})
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
    std::string_view name;
    core::member_goal (*goal)();
    stop_reason reason;
  };
  std::vector<shape> const shapes{
      {"budget",  [] { return core::member_goal{.budget = icount{300}}; },                    stop_reason::budget},
      {"address", [] { return core::member_goal{.until = scratch_code + 2 * 400}; },
       stop_reason::address                                                                                      },
      {"time",    [] { return core::member_goal{.until_time = core::sys_time{60 * us_ps}}; },
       stop_reason::time                                                                                         },
  };
  constexpr std::int64_t arrival = 200 * 250'000;
  for(auto const& [name, wanted, reason] : shapes)
  {
    std::optional<std::vector<std::uint64_t>> reference;
    for(auto const latency : {20 * us_ps, 200 * us_ps})
    {
      for(auto const& how : {variants()[0], variants()[1], variants()[7], variants()[8]})
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
        auto       goal    = r.goal(1, wanted());
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
  for(auto const& how : {variants()[0], variants()[1], variants()[2], variants()[7], variants()[8]})
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
      for(auto const& how : {variants()[0], variants()[1], variants()[7], variants()[8]})
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
  for(auto const& how :
      {variants()[0], variants()[1], variants()[2], variants()[4], variants()[7], variants()[8]})
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
  for(auto const& how : {variants()[0], variants()[1], variants()[7], variants()[8]})
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

TEST_CASE("base_of is read from another thread while a member resets mid-run")
{
  auto resetting     = member("calls", "reset_now");
  resetting.on_reset = popcorn::core::reset_request_policy::reset_and_continue;
  rig r{
      .members = {member("alu", "alu_entry"), resetting}
  };
  r.build(variants()[0]);
  auto const                 first  = r.sys->base_of(0).ps;
  auto const                 before = r.sys->base_of(1).ps;
  std::atomic<bool>          done{};
  std::vector<std::int64_t>  firsts{first};
  std::vector<std::int64_t>  seconds{before};
  std::atomic<std::uint64_t> reads{};
  std::jthread               reader{[&] {
    while(not done.load(std::memory_order_acquire))
    {
      reads.fetch_add(1, std::memory_order_release);
      if(auto const one = r.sys->base_of(0).ps; one != firsts.back())
      {
        firsts.push_back(one);
      }
      if(auto const one = r.sys->base_of(1).ps; one != seconds.back())
      {
        seconds.push_back(one);
      }
    }
  }};
  while(reads.load(std::memory_order_acquire) == 0)
  {
    std::this_thread::yield();
  }
  auto       goal    = r.goal(1, {.budget = icount{5000}});
  auto const stopped = r.run({});
  REQUIRE(stopped.reason == core::pause_reason::delivered);
  CHECK(goal.get().stopped.reason == stop_reason::budget);
  done.store(true, std::memory_order_release);
  reader.join();
  auto const after = r.sys->base_of(1).ps;
  CHECK(after != before);
  CHECK(r.links[0]->receiver_resets.size() == 1);
  CHECK(std::ranges::all_of(firsts, [&](std::int64_t one) { return one == first; }));
  CHECK(std::ranges::all_of(seconds, [&](std::int64_t one) {
    return one == before or one == after;
  }));
  r.sys->close();
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
  for(auto const& how :
      {variants()[0], variants()[1], variants()[2], variants()[4], variants()[7], variants()[8]})
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
    r.sys = std::make_unique<core::system>(r.members, std::move(wires), how.bounds);
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
  for(auto const& how : {variants()[0], variants()[1], variants()[2], variants()[7], variants()[8]})
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
    r.sys = std::make_unique<core::system>(r.members, std::move(wires), how.bounds);
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
  std::optional<std::vector<std::uint64_t>>              reference;
  std::optional<std::vector<std::uint64_t>>              clocked;
  std::optional<std::pair<std::uint64_t, std::uint64_t>> landing;
  for(auto const& how :
      {variants()[0], variants()[1], variants()[2], variants()[6], variants()[7], variants()[8]})
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
    auto& same = how.mode == core::sync_mode::barrier ? reference : clocked;
    if(same)
    {
      CHECK(dump == *same);
    }
    same            = dump;
    auto const pair = std::pair{landed.get().ps, logged};
    if(landing)
    {
      CHECK(pair == *landing);
    }
    landing = pair;
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
    std::atomic<bool>                          woken{false};
    core::system*                              owner = nullptr;
    std::move_only_function<void(std::size_t)> hook;
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
        core::slice_bounds{}, std::move_only_function<void(std::size_t)>{}, std::move(hook));
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

TEST_CASE("an arrival hook that throws pauses the system on that error once")
{
  for(auto const& how : variants())
  {
    if(how.seed)
    {
      continue;
    }
    for(bool const with_goal : {false, true})
    {
      CAPTURE(how.name);
      CAPTURE(with_goal);
      pinned const             pin{how.one_cpu};
      std::atomic<std::size_t> thrown{0};
      rig                      r{.members = sleeper_and_runner()};
      r.arrival = [&thrown](std::size_t index) {
        if(index == 1)
        {
          ++thrown;
          throw test_failure("arrival hook");
        }
      };
      r.build(how);
      std::optional<core::host_future<core::goal_reached>> goal;
      if(with_goal)
      {
        goal = r.goal(1, {.budget = icount{1'000'000'000}});
      }
      auto const stopped = r.run(2 * ms_ps);
      REQUIRE(stopped.reason == core::pause_reason::error);
      CHECK(stopped.member == 1);
      CHECK_THROWS_AS(std::rethrow_exception(stopped.error), test_failure);
      CHECK(thrown.load() == 1);
      auto const quantum = how.lowered != 0 ? 20 * us_ps / how.lowered : 20 * us_ps;
      CHECK(stopped.at.ps >= quantum);
      if(how.mode == core::sync_mode::barrier)
      {
        CHECK(stopped.at.ps < 2 * quantum);
      }
      {
        auto const held = r.sys->lock();
        CHECK(r.sys->time_of(held, 1) == stopped.at);
      }
      if(goal)
      {
        CHECK_THROWS_AS(std::ignore = goal->get(), test_failure);
      }
      CHECK(parked(r.members[0]));
      CHECK(parked(r.members[1]));
    }
  }
}

TEST_CASE("a sleep hook that throws pauses the system on that error once")
{
  rig r{
      .members = {sleeper_with_event(400), sleeper_with_event(900)}
  };
  r.links.push_back(std::make_shared<fake_link>(0, 1, 20 * us_ps));
  std::atomic<std::size_t> thrown{0};
  r.sys = std::make_unique<core::system>(
      r.members, std::vector<std::shared_ptr<core::member_link>>{r.links[0]}, core::slice_bounds{},
      std::move_only_function<void(std::size_t)>{}, [&thrown](std::size_t index) {
        if(index == 1)
        {
          ++thrown;
          throw test_failure("sleep hook");
        }
      });
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
  auto const stopped = pending.get();
  REQUIRE(stopped.reason == core::pause_reason::error);
  CHECK(stopped.member == 1);
  CHECK_THROWS_AS(std::rethrow_exception(stopped.error), test_failure);
  CHECK(thrown.load() == 1);
  CHECK_THROWS_AS(std::ignore = b.get(), test_failure);
  CHECK(parked(r.members[0]));
  CHECK(parked(r.members[1]));
}

TEST_CASE("save refuses a pending goal and an undecided member")
{
  rig r{
      .members = {member("alu", "alu_entry"), member("alu", "alu_entry")}
  };
  r.build(variants()[0]);
  std::vector<std::byte> out;
  auto                   goal = r.goal(0, {.budget = icount{100}});
  {
    auto const held = r.sys->lock();
    CHECK_THROWS_AS(r.sys->save(held, out), std::logic_error);
  }
  REQUIRE(r.run({}).member == 0);
  {
    auto const held = r.sys->lock();
    CHECK_THROWS_AS(r.sys->save(held, out), std::logic_error);
  }
  r.release(0);
  auto const held = r.sys->lock();
  r.sys->save(held, out);
  CHECK_FALSE(out.empty());
}

TEST_CASE("a reset decides a member held at its stop, which runs on the next system time resume")
{
  for(bool const released : {false, true})
  {
    CAPTURE(released);
    rig r{
        .members = {member("alu", "alu_entry"), member("alu", "alu_entry")}
    };
    r.build(variants()[0]);
    auto goal = r.goal(0, {.budget = icount{100}});
    REQUIRE(r.run({}).member == 0);
    if(released)
    {
      r.release(0);
    }
    std::vector<std::byte> out;
    {
      auto const held = r.sys->lock();
      r.sys->reset(held, 0, popcorn::reset_kind::software);
      CHECK(r.sys->undecided(held).empty());
      r.sys->save(held, out);
    }
    auto const before = [&r] {
      auto const held = r.sys->lock();
      return r.sys->executed_of(held, 0).value;
    }();
    REQUIRE(r.run(200 * us_ps).reason == core::pause_reason::system_time);
    auto const held = r.sys->lock();
    CHECK(r.sys->executed_of(held, 0).value > before);
  }
}

TEST_CASE("a release halts its member only when asked, and load restores which it did")
{
  for(bool const halt : {false, true})
  {
    CAPTURE(halt);
    rig r{
        .members = {member("alu", "alu_entry"), member("alu", "alu_entry")}
    };
    r.build(variants()[0]);
    auto const executed = [&r] {
      auto const held = r.sys->lock();
      return r.sys->executed_of(held, 0).value;
    };
    auto goal = r.goal(0, {.budget = icount{100}});
    {
      auto const held = r.sys->lock();
      CHECK_THROWS_AS(r.sys->release(held, 0, true), std::logic_error);
    }
    REQUIRE(r.run({}).member == 0);
    {
      auto const held = r.sys->lock();
      CHECK_THROWS_AS(r.sys->release(held, 1, false), std::logic_error);
    }
    r.release(0, halt);
    std::vector<core::snapshot> saved;
    for(auto const& one : r.members)
    {
      saved.push_back(core::access(*one.on, 0, [](core::machine& m) { return m.snapshot(); }));
    }
    std::vector<std::byte> state;
    {
      auto const held = r.sys->lock();
      r.sys->save(held, state);
    }
    auto const before = executed();
    REQUIRE(r.run(200 * us_ps).reason == core::pause_reason::system_time);
    CHECK((executed() > before) == not halt);
    if(halt)
    {
      auto again = r.goal(0, {.budget = icount{100}});
      REQUIRE(r.run({}).member == 0);
      r.release(0, false);
    }
    else
    {
      r.release(0, true);
    }
    auto const flipped = executed();
    REQUIRE(r.run(400 * us_ps).reason == core::pause_reason::system_time);
    CHECK((executed() > flipped) == halt);
    {
      auto const held = r.sys->lock();
      for(std::size_t index = 0; index < 2; ++index)
      {
        core::access(*r.members[index].on, 0, [&](core::machine& m) {
          m.restore(core::snapshot{saved[index].data()});
        });
      }
      r.sys->load(held, state);
    }
    auto const loaded = executed();
    REQUIRE(r.run(600 * us_ps).reason == core::pause_reason::system_time);
    CHECK((executed() > loaded) == not halt);
  }
}

TEST_CASE("executed counts what a core ran, not its sleep or halt, is saved and zeroed by a reset")
{
  std::optional<std::vector<std::uint64_t>> reference;
  for(auto const& how : variants())
  {
    CAPTURE(how.name);
    pinned const pin{how.one_cpu};
    rig          r{
        .members = {member("idle", "idle_forever"), member("alu", "alu_entry")}
    };
    r.build(how);
    struct counts
    {
      std::uint64_t executed{};
      std::uint64_t now{};
      std::uint64_t system{};
    };
    auto const read = [&r](std::size_t index) {
      auto const held = r.sys->lock();
      return core::access(*r.members[index].on, 0, [&](core::machine& m) {
        return counts{
            .executed = m.executed().value,
            .now      = m.virtual_clock().now().value,
            .system   = r.sys->executed_of(held, index).value};
      });
    };
    REQUIRE(r.run(200 * us_ps).reason == core::pause_reason::system_time);
    auto const sleeper = read(0);
    auto const runner  = read(1);
    CHECK(sleeper.executed == sleeper.system);
    CHECK(sleeper.executed < sleeper.now / 4);
    CHECK(runner.executed == runner.system);
    CHECK(runner.executed > runner.now / 2);

    std::vector<core::snapshot> saved;
    for(auto const& one : r.members)
    {
      saved.push_back(core::access(*one.on, 0, [](core::machine& m) { return m.snapshot(); }));
    }
    std::vector<std::byte> state;
    {
      auto const held = r.sys->lock();
      r.sys->save(held, state);
    }
    auto goal = r.goal(1, {.budget = icount{100}});
    REQUIRE(r.run({}).member == 1);
    r.release(1, true);
    auto const halted = read(1);
    CHECK(halted.executed > runner.executed);
    REQUIRE(r.run(400 * us_ps).reason == core::pause_reason::system_time);
    auto const after = read(1);
    CHECK(after.executed == halted.executed);
    CHECK(after.now > halted.now);
    std::vector<std::uint64_t> seen{
        sleeper.executed, runner.executed, halted.executed, after.executed};
    if(reference)
    {
      CHECK(seen == *reference);
    }
    reference = seen;
    {
      auto const held = r.sys->lock();
      for(std::size_t index = 0; index < 2; ++index)
      {
        core::access(*r.members[index].on, 0, [&](core::machine& m) {
          m.restore(core::snapshot{saved[index].data()});
        });
      }
      r.sys->load(held, state);
    }
    CHECK(
        core::access(*r.members[1].on, 0, [](core::machine& m) { return m.executed().value; })
        == runner.executed);
    {
      auto const held = r.sys->lock();
      r.sys->reset(held, 1, popcorn::reset_kind::software);
    }
    CHECK(
        core::access(*r.members[1].on, 0, [](core::machine& m) { return m.executed().value; })
        == 0);
  }
}

TEST_CASE("a load after an error pause clears the error and the system resumes")
{
  rig r{
      .members = {
                  member("alu", "alu_entry"),
                  member<throwing_arch>(
              "idle", "idle_budget", [](popcorn::testing::machine_fixture<throwing_arch>& f) {
                std::ignore = f.module->attached_events().schedule(
                    icount{1777}, throwing_arch::throw_event, 0);
              })}
  };
  r.build(variants()[0]);
  REQUIRE(r.run(200 * us_ps).reason == core::pause_reason::system_time);
  std::vector<core::snapshot> saved;
  std::vector<std::byte>      state;
  for(auto const& one : r.members)
  {
    saved.push_back(core::access(*one.on, 0, [](core::machine& m) { return m.snapshot(); }));
  }
  {
    auto const held = r.sys->lock();
    r.sys->save(held, state);
  }
  auto const first = r.run(2 * ms_ps);
  REQUIRE(first.reason == core::pause_reason::error);
  {
    auto const held = r.sys->lock();
    CHECK_THROWS_AS(std::ignore = r.sys->resume(held, core::sys_time{3 * ms_ps}), std::logic_error);
    for(std::size_t index = 0; index < 2; ++index)
    {
      core::access(*r.members[index].on, 0, [&](core::machine& m) {
        m.restore(core::snapshot{saved[index].data()});
      });
    }
    r.sys->load(held, state);
    CHECK(r.sys->time_of(held, 1).ps == 200 * us_ps);
  }
  auto const again = r.run(2 * ms_ps);
  REQUIRE(again.reason == core::pause_reason::error);
  CHECK(again.at == first.at);
  CHECK(again.member == first.member);
}

TEST_CASE("a state check accepts what load accepts and changes nothing")
{
  rig r{.members = sleeper_and_runner()};
  r.build(variants()[0]);
  REQUIRE(r.run(200 * us_ps).reason == core::pause_reason::system_time);
  r.links[0]->state = std::byte{7};
  std::vector<std::byte> earlier;
  {
    auto const held = r.sys->lock();
    r.sys->save(held, earlier);
  }
  REQUIRE(r.run(1 * ms_ps).reason == core::pause_reason::system_time);
  r.links[0]->state           = std::byte{9};
  auto const             held = r.sys->lock();
  std::vector<std::byte> now;
  r.sys->save(held, now);
  r.sys->check_state(held, earlier);
  std::vector<std::byte> after;
  r.sys->save(held, after);
  CHECK(after == now);
  std::vector<std::byte> const junk{std::byte{1}, std::byte{2}};
  CHECK_THROWS_AS(r.sys->check_state(held, junk), popcorn::config_error);
}

TEST_CASE("a load that fails on its second link leaves the system as it was")
{
  rig r{.members = sleeper_and_runner()};
  r.build(variants()[1]);
  REQUIRE(r.run(100 * us_ps).reason == core::pause_reason::system_time);
  r.links[0]->state = std::byte{1};
  r.links[1]->state = std::byte{2};
  std::vector<std::byte> saved;
  {
    auto const held = r.sys->lock();
    r.sys->save(held, saved);
  }
  REQUIRE(r.run(300 * us_ps).reason == core::pause_reason::system_time);
  r.links[0]->state     = std::byte{5};
  r.links[1]->fail_load = true;
  auto const held       = r.sys->lock();
  auto const barrier    = r.sys->pending_barrier(held);
  CHECK_THROWS_AS(r.sys->load(held, saved), popcorn::config_error);
  CHECK(r.links[0]->state == std::byte{5});
  CHECK(r.sys->pending_barrier(held) == barrier);
}

namespace
{
struct member_mirror
{
  core::sys_time                base{};
  std::optional<core::sys_time> held_to{};
  bool                          released{};
};

struct system_mirror
{
  std::uint32_t                       format{};
  std::uint64_t                       window{};
  core::sys_time                      pending{};
  std::optional<core::sys_time>       last_point{};
  std::vector<member_mirror>          members{};
  std::vector<std::vector<std::byte>> links{};
};
}

TEST_CASE("a system state of format 2 loads, one of format 1 is refused")
{
  rig r{.members = sleeper_and_runner()};
  r.build(variants()[0]);
  REQUIRE(r.run(100 * us_ps).reason == core::pause_reason::system_time);
  auto const             held = r.sys->lock();
  std::vector<std::byte> saved;
  r.sys->save(held, saved);
  auto state =
      reflex::serde::bson::deserializer{std::span<std::byte const>{saved}}.load<system_mirror>();
  REQUIRE(state.format == 2);
  std::vector<std::byte> again;
  reflex::serde::bson::serializer{again}.dump(state);
  CHECK(again == saved);
  r.sys->load(held, again);
  state.format = 1;
  std::vector<std::byte> old;
  reflex::serde::bson::serializer{old}.dump(state);
  CHECK_THROWS_WITH_AS(
      r.sys->load(held, old), doctest::Contains("this one has format 1"), popcorn::config_error);
}

namespace
{
struct stamp_seen
{
  std::vector<std::uint64_t> log;
  std::int64_t               at{};
};

auto stamp_after_pause(bool reload) -> stamp_seen
{
  popcorn::testing::test_arch* module = nullptr;
  auto                         make   = [&module] {
    return member<popcorn::testing::test_arch>(
        "idle", "idle_forever", [&module](popcorn::testing::machine_fixture<>& f) {
          auto const now = f.target->virtual_clock().now().value;
          std::ignore    = f.module->attached_events().schedule(icount{now + 103}, logged_kind, 7);
          module         = f.module;
        });
  };
  rig r{
      .members = {member("alu", "alu_entry"), make()}
  };
  r.build(variants()[0]);
  auto       goal    = r.goal(0, {.budget = icount{100}});
  auto const stopped = r.run({});
  REQUIRE(stopped.member == 0);
  r.release(0);
  REQUIRE(r.now_of(1) == stopped.at.ps);
  if(reload)
  {
    std::vector<core::snapshot> saved;
    std::vector<std::byte>      state;
    for(auto const& one : r.members)
    {
      saved.push_back(core::access(*one.on, 0, [](core::machine& m) { return m.snapshot(); }));
    }
    {
      auto const held = r.sys->lock();
      r.sys->save(held, state);
    }
    rig fresh{
        .members = {member("alu", "alu_entry"), make()}
    };
    fresh.build(variants()[0]);
    for(std::size_t index = 0; index < 2; ++index)
    {
      core::access(*fresh.members[index].on, 0, [&](core::machine& m) {
        m.restore(core::snapshot{saved[index].data()});
      });
    }
    {
      auto const held = fresh.sys->lock();
      fresh.sys->load(held, state);
    }
    r = std::move(fresh);
  }
  auto*                      log = &module->event_log;
  std::vector<std::uint64_t> seen;
  core::host_future<vtime>   landed;
  {
    auto const held_host = r.members[1].on->lock();
    auto const now       = r.members[1].on->member(held_host, 0).virtual_clock().time();
    landed               = r.members[1].on->post(
        held_host, 0,
        [&seen, log](core::machine&) {
          for(auto const& one : *log)
          {
            seen.push_back(one.payload);
          }
        },
        vtime{now.ps});
  }
  if(not landed.done())
  {
    REQUIRE(r.run({}).reason == core::pause_reason::idle);
  }
  return {.log = seen, .at = static_cast<std::int64_t>(landed.get().ps)};
}
}

TEST_CASE("a stamp at a pause point with an event held there runs after it, through save and load")
{
  auto const plain    = stamp_after_pause(false);
  auto const reloaded = stamp_after_pause(true);
  CHECK(plain.log == std::vector<std::uint64_t>{7});
  CHECK(reloaded.log == plain.log);
  CHECK(reloaded.at == plain.at);
}

TEST_CASE("a lone machine stopped between two same-count events runs a stamp at its time at once")
{
  popcorn::testing::test_arch* module = nullptr;
  auto                         lone   = member<popcorn::testing::test_arch>(
      "idle", "idle_forever", [&module](popcorn::testing::machine_fixture<>& f) {
        auto const now = f.target->virtual_clock().now().value;
        std::ignore    = f.module->attached_events().schedule(icount{now + 50}, logged_kind, 1);
        std::ignore    = f.module->attached_events().schedule(icount{now + 50}, logged_kind, 2);
        module         = f.module;
      });
  core::host_future<core::goal_result> stopped;
  {
    auto const held = lone.on->lock();
    stopped =
        lone.on->run(held, 0, {.stop_when = [&module] { return not module->event_log.empty(); }});
  }
  REQUIRE(stopped.get().stopped.reason == stop_reason::user);
  auto const held = lone.on->lock();
  auto&      m    = lone.on->member(held, 0);
  REQUIRE(m.next_event());
  REQUIRE(*m.next_event() <= m.virtual_clock().now());
  auto const landed = lone.on->post(held, 0, [](core::machine&) {}, m.virtual_clock().time());
  CHECK(landed.done());
}

TEST_CASE("equal stamps on a linked member keep their order when the due event is cancelled")
{
  popcorn::testing::test_arch* module = nullptr;
  std::uint64_t                due_id = 0;
  auto                         make   = [&module, &due_id] {
    return member<popcorn::testing::test_arch>(
        "idle", "idle_forever", [&module, &due_id](popcorn::testing::machine_fixture<>& f) {
          auto const now = f.target->virtual_clock().now().value;
          due_id         = f.module->attached_events().schedule(icount{now + 103}, logged_kind, 7);
          module         = f.module;
        });
  };
  rig r{
      .members = {member("alu", "alu_entry"), make()}
  };
  r.build(variants()[0]);
  auto goal = r.goal(0, {.budget = icount{100}});
  REQUIRE(r.run({}).member == 0);
  r.release(0);
  std::vector<int>         order;
  core::host_future<vtime> first;
  core::host_future<vtime> second;
  {
    auto const held_host = r.members[1].on->lock();
    auto&      m         = r.members[1].on->member(held_host, 0);
    auto const now       = m.virtual_clock().time();
    first =
        r.members[1].on->post(held_host, 0, [&order](core::machine&) { order.push_back(1); }, now);
    CHECK_FALSE(first.done());
    module->attached_events().cancel(due_id);
    second =
        r.members[1].on->post(held_host, 0, [&order](core::machine&) { order.push_back(2); }, now);
  }
  CHECK_FALSE(second.done());
  REQUIRE(r.run({}).reason == core::pause_reason::idle);
  std::ignore = first.get();
  std::ignore = second.get();
  CHECK(order == std::vector<int>{1, 2});
}

namespace
{
auto lone_stamp_after(bool through_snapshot) -> std::vector<std::uint64_t>
{
  popcorn::testing::test_arch* module = nullptr;
  auto                         make   = [&module] {
    return member<popcorn::testing::test_arch>(
        "idle", "idle_forever", [&module](popcorn::testing::machine_fixture<>& f) {
          auto const now = f.target->virtual_clock().now().value;
          std::ignore    = f.module->attached_events().schedule(icount{now + 103}, logged_kind, 7);
          module         = f.module;
        });
  };
  rig r{
      .members = {member("alu", "alu_entry"), make()}
  };
  r.build(variants()[0]);
  auto goal = r.goal(0, {.budget = icount{100}});
  REQUIRE(r.run({}).member == 0);
  r.release(0);
  auto lone = r.members[1];
  if(through_snapshot)
  {
    auto const saved =
        core::access(*lone.on, 0, [](core::machine& m) { return m.snapshot().data(); });
    r.sys.reset();
    lone = make();
    core::access(*lone.on, 0, [&saved](core::machine& m) { m.restore(core::snapshot{saved}); });
  }
  else
  {
    r.sys.reset();
  }
  auto*                      log = &module->event_log;
  std::vector<std::uint64_t> seen;
  core::host_future<vtime>   landed;
  vtime                      at{};
  {
    auto const held = lone.on->lock();
    at              = lone.on->member(held, 0).virtual_clock().time();
    landed          = lone.on->post(
        held, 0,
        [&seen, log](core::machine&) {
          for(auto const& one : *log)
          {
            seen.push_back(one.payload);
          }
        },
        at);
  }
  core::host_future<core::goal_result> plain;
  {
    auto const held = lone.on->lock();
    plain           = lone.on->run(held, 0, {.until_time = vtime{at.ps + us_ps}});
  }
  std::ignore = plain.get();
  CHECK(landed.get() == at);
  return seen;
}
}

TEST_CASE("a stamp at a held point runs after the held event on a member left alone, saved or not")
{
  CHECK(lone_stamp_after(false) == std::vector<std::uint64_t>{7});
  CHECK(lone_stamp_after(true) == std::vector<std::uint64_t>{7});
}

TEST_CASE("a dispatch cut short after one held event keeps a stamp behind the next one")
{
  popcorn::testing::test_arch* module = nullptr;
  std::uint64_t                due    = 0;
  auto                         lone   = member<popcorn::testing::test_arch>(
      "idle", "idle_forever", [&module, &due](popcorn::testing::machine_fixture<>& f) {
        due         = f.target->virtual_clock().now().value + 50;
        std::ignore = f.module->attached_events().schedule(icount{due}, logged_kind, 1);
        std::ignore = f.module->attached_events().schedule(icount{due}, logged_kind, 2);
        module      = f.module;
      });
  std::vector<std::uint64_t> seen;
  core::host_future<vtime>   landed;
  {
    auto const held  = lone.on->lock();
    auto&      m     = lone.on->member(held, 0);
    auto const until = vtime{m.virtual_clock().time().ps + 50 * 250'000};
    REQUIRE(m.run_slice({.max_instructions = icount{1'000'000},
                         .until_time       = until,
                         .hold_from_until  = true,
                         .internal_stop    = true})
                .stopped);
    REQUIRE(m.virtual_clock().now().value == due);
    REQUIRE(module->event_log.empty());
    auto const cut = m.run_slice(
        {.max_instructions = icount{1'000'000}, .until_time = until, .stop_when = [&module] {
           return not module->event_log.empty();
         }});
    REQUIRE(cut.stopped);
    REQUIRE(cut.stopped->reason == stop_reason::user);
    REQUIRE(module->event_log.size() == 1);
    landed = lone.on->post(
        held, 0,
        [&seen, &module](core::machine&) {
          for(auto const& one : module->event_log)
          {
            seen.push_back(one.payload);
          }
        },
        m.virtual_clock().time());
    CHECK_FALSE(landed.done());
  }
  core::host_future<core::goal_result> plain;
  {
    auto const held = lone.on->lock();
    plain           = lone.on->run(held, 0, {.budget = icount{1}});
  }
  std::ignore = plain.get();
  std::ignore = landed.get();
  CHECK(seen == std::vector<std::uint64_t>{1, 2});
}

TEST_CASE_TEMPLATE(
    "a time compared under its lock is read through an instrumented operator",
    T,
    core::sys_time,
    vtime,
    icount)
{
  constexpr std::uint32_t stores = 20'000;
  std::mutex              guard;
  T                       shared{0U};
  std::latch              start{2};
  std::thread             writer{[&] {
    start.arrive_and_wait();
    for(std::uint32_t n = 1; n <= stores; ++n)
    {
      std::scoped_lock const held{guard};
      shared = T{n};
    }
  }};
  start.arrive_and_wait();
  std::uint32_t seen      = 0;
  std::uint32_t backwards = 0;
  while(seen < stores)
  {
    std::scoped_lock const held{guard};
    if(shared < T{seen})
    {
      ++backwards;
    }
    while(T{seen} < shared)
    {
      ++seen;
    }
  }
  writer.join();
  CHECK(backwards == 0);
  CHECK(seen == stores);
}

#if defined(__SANITIZE_THREAD__)
extern "C"
{
  void __sanitizer_set_report_path(char const* path);
  void __tsan_on_report(void* report);
}

namespace
{
std::atomic<std::uint32_t> tsan_reports{0};
}

extern "C" void __tsan_on_report(void*)
{
  tsan_reports.fetch_add(1, std::memory_order_relaxed);
}

TEST_CASE_TEMPLATE(
    "an unlocked time comparison racing a store is reported by TSAN",
    T,
    core::sys_time,
    vtime,
    icount)
{
  auto const child = fork();
  REQUIRE(child >= 0);
  if(child == 0)
  {
    __sanitizer_set_report_path("/dev/null");
    constexpr std::uint32_t stores = 20'000;
    T                       shared{0U};
    std::latch              start{2};
    std::thread             writer{[&] {
      start.arrive_and_wait();
      for(std::uint32_t n = 1; n <= stores; ++n)
      {
        shared = T{n};
      }
    }};
    start.arrive_and_wait();
    std::uint32_t backwards = 0;
    for(std::uint32_t n = 0; n < stores; ++n)
    {
      backwards += shared < T{n} ? 1U : 0U;
    }
    writer.join();
    std::ignore = backwards;
    _exit(tsan_reports.load() == 0 ? 0 : 66);
  }
  int status = 0;
  REQUIRE(waitpid(child, &status, 0) == child);
  REQUIRE(WIFEXITED(status));
  CHECK(WEXITSTATUS(status) == 66);
}
#endif

namespace
{
struct wire_spec
{
  std::size_t                from{};
  std::size_t                to{};
  std::int64_t               latency{};
  std::optional<std::size_t> owner{};
};

struct aligned
{
  delivery                  first{};
  std::vector<std::int64_t> times{};
  std::vector<std::int64_t> executed{};
  std::optional<delivery>   second{};

  friend auto operator==(aligned const&, aligned const&) -> bool = default;
};

auto clock_variants() -> std::vector<variant>
{
  auto all = variants();
  std::erase_if(all, [](variant const& how) { return how.mode != core::sync_mode::clocks; });
  return all;
}

auto jitter(variant const& how, std::size_t members) -> std::move_only_function<void(std::size_t)>
{
  if(not how.seed)
  {
    return {};
  }
  auto engines = std::make_shared<std::vector<std::mt19937>>();
  for(std::size_t index = 0; index < members; ++index)
  {
    engines->emplace_back(*how.seed * 31 + index);
  }
  return [engines](std::size_t index) {
    std::uniform_int_distribution<int> pause{0, 50};
    std::this_thread::sleep_for(std::chrono::microseconds{pause((*engines)[index])});
  };
}

auto run_aligned(
    std::vector<core::member_ref>             members,
    std::vector<wire_spec> const&             specs,
    std::vector<std::optional<std::uint32_t>> stops,
    variant const&                            how,
    bool                                      again  = false,
    std::vector<std::size_t> const&           halted = {}) -> aligned
{
  pinned const                                    pin{how.one_cpu};
  std::vector<std::shared_ptr<core::member_link>> wires;
  for(auto const& one : specs)
  {
    wires.push_back(std::make_shared<fake_link>(one.from, one.to, one.latency));
  }
  core::system sys{
      members,
      std::move(wires),
      how.bounds,
      jitter(how, members.size()),
      {},
      how.mode,
      core::system::default_lookahead,
      core::sys_time{how.progress}};
  std::vector<core::host_future<core::goal_reached>> goals;
  {
    auto const                  held = sys.lock();
    std::vector<core::sys_time> offsets(members.size());
    sys.start(held, offsets);
    for(std::size_t index = 0; index < specs.size(); ++index)
    {
      if(specs[index].owner)
      {
        sys.set_cut_owner(held, index, *specs[index].owner);
      }
    }
    for(std::size_t index = 0; index < members.size(); ++index)
    {
      if(std::ranges::contains(halted, index))
      {
        continue;
      }
      core::member_goal goal{};
      if(stops[index])
      {
        goal.until = popcorn::address{*stops[index]};
      }
      goals.push_back(sys.post_goal(held, index, std::move(goal)));
    }
  }
  auto const resume = [&sys] {
    core::host_future<core::pause> pending;
    {
      auto const held = sys.lock();
      pending         = sys.resume(held);
    }
    return pending.get();
  };
  aligned out{.first = delivered(resume())};
  {
    auto const held = sys.lock();
    for(std::size_t index = 0; index < members.size(); ++index)
    {
      out.times.push_back(sys.time_of(held, index).ps);
      out.executed.push_back(static_cast<std::int64_t>(sys.executed_of(held, index).value));
    }
    sys.release(held, out.first.member, true);
  }
  if(again)
  {
    out.second = delivered(resume());
  }
  sys.close();
  return out;
}

auto every_run(std::function_ref<aligned(variant const&)> one) -> aligned
{
  std::optional<aligned> reference;
  for(auto const& how : clock_variants())
  {
    for(int repeat = 0; repeat < 5; ++repeat)
    {
      CAPTURE(how.name);
      CAPTURE(repeat);
      auto const seen = one(how);
      if(reference)
      {
        CHECK(seen == *reference);
      }
      else
      {
        reference = seen;
      }
    }
  }
  return *reference;
}

void check_at(std::int64_t seen, std::int64_t at)
{
  CHECK(seen >= at);
  CHECK(seen < at + 512 * instruction_ps);
}
}

TEST_CASE("after a member stop a linked member stands at the stop plus L in clock mode")
{
  auto const done = sym("alu", "alu_done");
  auto const run  = every_run([&](variant const& how) {
    return run_aligned(
        {
            alu_counting(2000), alu_counting(1'000'000)
    },
        {{.from = 0, .to = 1, .latency = 20 * us_ps}}, {done, done}, how);
  });
  CHECK(run.first.member == 0);
  CHECK(run.first.reason == static_cast<int>(core::pause_reason::delivered));
  CHECK(run.times[0] == run.first.at);
  check_at(run.times[1], run.first.at + 20 * us_ps);
}

TEST_CASE("after a member stop the end of a chain stands at the stop plus both latencies")
{
  auto const done = sym("alu", "alu_done");
  auto const run  = every_run([&](variant const& how) {
    return run_aligned(
        {
            alu_counting(2000), alu_counting(1'000'000), alu_counting(1'000'000)
    },
        {{.from = 0, .to = 1, .latency = 20 * us_ps}, {.from = 1, .to = 2, .latency = 30 * us_ps}},
        {done, done, done}, how);
  });
  CHECK(run.first.member == 0);
  check_at(run.times[1], run.first.at + 20 * us_ps);
  check_at(run.times[2], run.first.at + 50 * us_ps);
}

TEST_CASE(
    "after a member stop an unlinked pair stands at the stop plus Q through its synthetic edge")
{
  auto const done = sym("alu", "alu_done");
  auto const run  = every_run([&](variant const& how) {
    return run_aligned(
        {
            alu_counting(2000), alu_counting(1'000'000), alu_counting(1'000'000),
            alu_counting(1'000'000)
    },
        {{.from = 0, .to = 1, .latency = 20 * us_ps}, {.from = 2, .to = 3, .latency = 50 * us_ps}},
        {done, done, done, done}, how);
  });
  CHECK(run.first.member == 0);
  check_at(run.times[1], run.first.at + 20 * us_ps);
  check_at(run.times[2], run.first.at + 20 * us_ps);
  check_at(run.times[3], run.first.at + 20 * us_ps);
}

TEST_CASE("after its stop an off-link cut owner that never cuts holds both ends at its stop plus L")
{
  auto const done = sym("alu", "alu_done");
  auto const run  = every_run([&](variant const& how) {
    return run_aligned(
        {
            alu_counting(1'000'000), alu_counting(1'000'000), alu_counting(2000)
    },
        {{.from = 0, .to = 1, .latency = 20 * us_ps, .owner = 2},
         {.from = 1, .to = 0, .latency = 20 * us_ps, .owner = 2},
         {.from = 0, .to = 2, .latency = 5 * us_ps}},
        {done, done, done}, how);
  });
  CHECK(run.first.member == 2);
  check_at(run.times[0], run.first.at + 20 * us_ps);
  check_at(run.times[1], run.first.at + 20 * us_ps);
}

TEST_CASE("after a member stop a quiescent member jumps to its aligned point executing nothing")
{
  auto const done = sym("alu", "alu_done");
  auto const run  = every_run([&](variant const& how) {
    return run_aligned(
        {
            alu_counting(2000), member("idle", "idle_forever")
    },
        {{.from = 0, .to = 1, .latency = 20 * us_ps}}, {done, std::nullopt}, how);
  });
  CHECK(run.first.member == 0);
  CHECK(run.times[1] >= run.first.at + 20 * us_ps);
  CHECK(run.times[1] < run.first.at + 20 * us_ps + instruction_ps);
  CHECK(run.executed[1] < 100);
}

TEST_CASE(
    "a member whose own stop comes before its aligned point stands there, held, delivered next")
{
  auto const done = sym("alu", "alu_done");
  auto const run  = every_run([&](variant const& how) {
    return run_aligned(
        {
            alu_counting(2000), alu_counting(2002), alu_counting(1'000'000)
    },
        {{.from = 0, .to = 1, .latency = 20 * us_ps}, {.from = 1, .to = 2, .latency = 20 * us_ps}},
        {done, done, done}, how, true);
  });
  CHECK(run.first.member == 0);
  CHECK(run.times[1] > run.first.at);
  CHECK(run.times[1] < run.first.at + 20 * us_ps);
  check_at(run.times[2], run.times[1] + 20 * us_ps);
  REQUIRE(run.second);
  CHECK(run.second->member == 1);
  CHECK(run.second->at == run.times[1]);
}

TEST_CASE("a time goal whose slice overshoots a due bound past its time resolves at its time")
{
  std::int64_t cut{};
  std::int64_t past{};
  for(std::int64_t step = 0; step < 16 and past <= cut; ++step)
  {
    cut = 1 * ms_ps + step * instruction_ps;
    rig r{.members = sleeper_and_runner()};
    r.build(variants()[0]);
    REQUIRE(r.run(cut).reason == core::pause_reason::system_time);
    past = r.now_of(1);
  }
  REQUIRE(past > cut);
  for(auto const& how : clock_variants())
  {
    CAPTURE(how.name);
    pinned const                  pin{how.one_cpu};
    std::vector<core::member_ref> members{
        member("idle", "idle_forever"), member("alu", "alu_entry")};
    auto const inbound = std::make_shared<fake_link>(0, 1, 20 * us_ps);
    inbound->due_at    = cut;
    std::vector<std::shared_ptr<core::member_link>> wires{
        std::make_shared<fake_link>(1, 0, 20 * us_ps), inbound};
    core::system sys{
        members,
        std::move(wires),
        how.bounds,
        jitter(how, 2),
        {},
        how.mode,
        core::system::default_lookahead,
        core::sys_time{how.progress}};
    core::host_future<core::goal_reached> goal;
    core::host_future<core::pause>        pending;
    {
      auto const                  held = sys.lock();
      std::vector<core::sys_time> offsets(2);
      sys.start(held, offsets);
      goal    = sys.post_goal(held, 1, {.until_time = core::sys_time{past}});
      pending = sys.resume(held);
    }
    std::atomic<bool> landed{false};
    std::jthread      guard{[&sys, &landed](std::stop_token const& stopping) {
      auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
      while(not stopping.stop_requested() and std::chrono::steady_clock::now() < deadline)
      {
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
      }
      if(not landed.load())
      {
        sys.close();
      }
    }};
    auto const        stopped = pending.get();
    landed.store(true);
    guard.request_stop();
    REQUIRE(stopped.reason == core::pause_reason::delivered);
    CHECK(stopped.member == 1);
    CHECK(goal.get().stopped.reason == stop_reason::time);
    CHECK(stopped.at.ps == past);
  }
}

TEST_CASE("after a member stop two halted members stand at the stop, as in barrier mode")
{
  auto const done = sym("alu", "alu_done");
  auto const make = [&](variant const& how) {
    return run_aligned(
        {
            alu_counting(2000), alu_counting(1'000'000), member("idle", "idle_forever"),
            alu_counting(1'000'000)
    },
        {{.from = 0, .to = 1, .latency = 20 * us_ps}}, {done, done, std::nullopt, std::nullopt},
        how, false, {2, 3});
  };
  auto const barrier = make(variants()[0]);
  auto const run     = every_run(make);
  CHECK(run.first == barrier.first);
  CHECK(run.first.member == 0);
  CHECK(run.times[2] == barrier.times[2]);
  CHECK(run.times[3] == barrier.times[3]);
  CHECK(run.times[2] >= run.first.at);
  CHECK(run.times[2] < run.first.at + instruction_ps);
  CHECK(run.times[3] >= run.first.at);
  CHECK(run.times[3] < run.first.at + instruction_ps);
  check_at(run.times[1], run.first.at + 20 * us_ps);
}

TEST_CASE("after a member stop a halted receiver of the stopped member stands at the stop")
{
  auto const done = sym("alu", "alu_done");
  auto const make = [&](variant const& how) {
    return run_aligned(
        {
            alu_counting(2000), member("idle", "idle_forever")
    },
        {{.from = 0, .to = 1, .latency = 20 * us_ps}, {.from = 1, .to = 0, .latency = 20 * us_ps}},
        {done, std::nullopt}, how, false, {1});
  };
  auto const barrier = make(variants()[0]);
  auto const run     = every_run(make);
  CHECK(run.first == barrier.first);
  CHECK(run.times[1] == barrier.times[1]);
  CHECK(run.times[1] >= run.first.at);
  CHECK(run.times[1] < run.first.at + instruction_ps);
}

namespace
{
class scripted_link final : public core::member_link
{
public:
  struct hand
  {
    std::int64_t start{};
    std::int64_t position{};
    bool         dropped{};
    bool         wiped{};
  };

  scripted_link(std::int64_t latency, core::machine* receiver, std::vector<std::int64_t> starts)
      : starts{std::move(starts)}, latency_{latency}, receiver_{receiver}
  {}

  auto sender() const noexcept -> std::size_t override
  {
    return 0;
  }

  auto receiver() const noexcept -> std::size_t override
  {
    return 1;
  }

  auto latency() const noexcept -> core::sys_time override
  {
    return core::sys_time{latency_};
  }

  void enter_slice(core::time_map) override
  {
    entered_.fetch_add(1, std::memory_order_release);
  }

  auto posted_count() const noexcept -> std::uint64_t override
  {
    return entered_.load(std::memory_order_acquire);
  }

  void sender_reset(core::sys_time) override
  {}

  void receiver_reset(core::sys_time at) override
  {
    resets.push_back(at.ps);
    for(auto& one : hands)
    {
      if(not one.dropped and one.start + latency_ > at.ps)
      {
        one.wiped = true;
      }
    }
  }

  auto drain(
      std::uint64_t,
      core::sys_time const& barrier,
      core::sys_time const& next,
      core::time_map        receiver,
      core::sys_time const&) -> std::optional<core::link_fault> override
  {
    auto const position = receiver.to_system(receiver_->virtual_clock().time()).ps;
    past_position       = std::max(past_position, barrier.ps - position);
    if(next.ps - latency_ > position)
    {
      ++sender_ahead;
    }
    if(exact_from and barrier.ps >= *exact_from and barrier.ps != never_ps)
    {
      exact = barrier.ps;
      starts.insert(std::ranges::upper_bound(starts, barrier.ps), barrier.ps);
      exact_from.reset();
    }
    if(fault_after
       and barrier.ps >= *fault_after
       and next.ps - latency_ > barrier.ps
       and next.ps != never_ps)
    {
      fault_after.reset();
      fault       = barrier.ps + (next.ps - latency_ - barrier.ps) / 2;
      fault_reach = position;
      return core::link_fault{
          .at     = core::sys_time{*fault},
          .member = 1,
          .error  = std::make_exception_ptr(test_failure("scripted fault"))};
    }
    while(handed_ < starts.size() and starts[handed_] < barrier.ps)
    {
      auto const start   = starts[handed_++];
      auto const dropped = std::ranges::any_of(
          resets, [&](std::int64_t reset) { return start < reset and reset < start + latency_; });
      hands.push_back({.start = start, .position = position, .dropped = dropped});
    }
    return std::nullopt;
  }

  auto due() const -> std::optional<core::sys_time> override
  {
    if(handed_ < starts.size())
    {
      return core::sys_time{starts[handed_] + latency_};
    }
    return std::nullopt;
  }

  auto quiet() const -> bool override
  {
    return handed_ == starts.size();
  }

  void finish(core::sys_time) override
  {}

  void save(std::vector<std::byte>&) const override
  {}

  void load(std::span<std::byte const>) override
  {}

  [[nodiscard]] auto lost() const -> std::vector<std::int64_t>
  {
    std::vector<std::int64_t> out;
    for(auto const& one : hands)
    {
      if(one.dropped or one.wiped)
      {
        out.push_back(one.start);
      }
    }
    return out;
  }

  [[nodiscard]] auto latest_hand() const -> std::int64_t
  {
    std::int64_t worst = std::numeric_limits<std::int64_t>::min();
    for(auto const& one : hands)
    {
      worst = std::max(worst, one.position - one.start - latency_);
    }
    return worst;
  }

  static constexpr std::int64_t never_ps = std::numeric_limits<std::int64_t>::max();

  std::vector<std::int64_t>   starts;
  std::vector<std::int64_t>   resets{};
  std::vector<hand>           hands{};
  std::optional<std::int64_t> exact_from{};
  std::optional<std::int64_t> exact{};
  std::optional<std::int64_t> fault_after{};
  std::optional<std::int64_t> fault{};
  std::int64_t                fault_reach{};
  std::int64_t                past_position{std::numeric_limits<std::int64_t>::min()};
  std::uint64_t               sender_ahead{};

private:
  std::int64_t               latency_;
  core::machine*             receiver_;
  std::size_t                handed_{};
  std::atomic<std::uint64_t> entered_{};
};

struct scripted_run
{
  core::pause_reason          reason{};
  std::vector<std::int64_t>   lost;
  std::vector<std::int64_t>   resets;
  std::int64_t                late{};
  std::int64_t                past_position{};
  std::uint64_t               sender_ahead{};
  std::optional<std::int64_t> exact;
  std::optional<std::int64_t> fault;
  std::int64_t                fault_reach{};
  core::sys_time              at{};
  std::optional<std::size_t>  member;
  std::vector<std::int64_t>   times;
};

auto scripted(
    variant const&              how,
    std::string_view            receiver_entry,
    std::optional<std::int64_t> reset_at,
    std::optional<std::int64_t> exact_from,
    std::int64_t                until,
    std::optional<std::int64_t> fault_after = std::nullopt) -> scripted_run
{
  constexpr std::int64_t        latency = 20 * us_ps;
  pinned const                  pin{how.one_cpu};
  std::vector<core::member_ref> members{
      member("alu", "alu_entry"),
      member(receiver_entry == "alu_entry" ? "alu" : "idle", receiver_entry)};
  core::machine* target = nullptr;
  {
    auto const held_host = members[1].on->lock();
    target               = &members[1].on->member(held_host, 0);
  }
  std::vector<std::int64_t> starts;
  for(std::int64_t at = 3 * us_ps; at < until; at += 3 * us_ps)
  {
    starts.push_back(at);
  }
  auto const link   = std::make_shared<scripted_link>(latency, target, std::move(starts));
  link->exact_from  = exact_from;
  link->fault_after = fault_after;
  std::vector<std::shared_ptr<core::member_link>> wires{
      link, std::make_shared<fake_link>(1, 0, 3 * latency)};
  core::system sys{
      members,
      std::move(wires),
      how.bounds,
      [](std::size_t index) {
        if(index == 1)
        {
          std::this_thread::sleep_for(std::chrono::microseconds{100});
        }
      },
      {},
      how.mode,
      core::system::default_lookahead,
      core::sys_time{how.progress}};
  core::host_future<core::pause> pending;
  {
    auto const                  held = sys.lock();
    std::vector<core::sys_time> offsets(2);
    sys.start(held, offsets);
    if(reset_at)
    {
      auto const held_host = members[1].on->lock();
      std::ignore          = members[1].on->post(
          held_host, 0,
          [](core::machine& m) {
            REQUIRE(m.mem().write(0xE000'E00C, 4, popcorn::testing::test_arch::reset_key));
          },
          vtime{static_cast<std::uint64_t>(*reset_at)});
    }
    pending = sys.resume(held, core::sys_time{until});
  }
  std::atomic<bool> landed{false};
  std::jthread      guard{[&sys, &landed](std::stop_token const& stopping) {
    auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};
    while(not stopping.stop_requested() and std::chrono::steady_clock::now() < deadline)
    {
      std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    if(not landed.load())
    {
      sys.close();
    }
  }};
  auto const        stopped = pending.get();
  landed.store(true);
  guard.request_stop();
  std::vector<std::int64_t> times;
  if(stopped.reason != core::pause_reason::closed)
  {
    auto const held = sys.lock();
    times.push_back(sys.time_of(held, 0).ps);
    times.push_back(sys.time_of(held, 1).ps);
  }
  sys.close();
  return {
      .reason        = stopped.reason,
      .lost          = link->lost(),
      .resets        = link->resets,
      .late          = link->latest_hand(),
      .past_position = link->past_position,
      .sender_ahead  = link->sender_ahead,
      .exact         = link->exact,
      .fault         = link->fault,
      .fault_reach   = link->fault_reach,
      .at            = stopped.at,
      .member        = stopped.member,
      .times         = std::move(times)};
}
}

TEST_CASE(
    "a receiver reset while its sender runs ahead drops in clock mode what barrier mode drops")
{
  constexpr std::int64_t reset_at = 1 * ms_ps + 1'300'000;
  for(auto const* entry : {"alu_entry", "idle_forever"})
  {
    CAPTURE(entry);
    std::optional<std::pair<std::vector<std::int64_t>, std::vector<std::int64_t>>> reference;
    for(auto const& how : variants())
    {
      CAPTURE(how.name);
      auto const run = scripted(how, entry, reset_at, std::nullopt, 2 * ms_ps);
      REQUIRE(run.resets.size() == 1);
      auto const reset = run.resets[0];
      CHECK(reset >= reset_at);
      CHECK(reset < reset_at + 4 * us_ps);
      CHECK_FALSE(run.lost.empty());
      CHECK(std::ranges::all_of(run.lost, [reset](std::int64_t start) {
        return start < reset and reset < start + 20 * us_ps;
      }));
      if(std::string_view{entry} == "alu_entry")
      {
        CHECK(run.reason == core::pause_reason::system_time);
        CHECK(run.past_position <= 0);
        if(how.mode == core::sync_mode::clocks)
        {
          CHECK(run.sender_ahead > 0);
        }
      }
      std::pair seen{run.resets, run.lost};
      if(reference)
      {
        CHECK(seen == *reference);
      }
      else
      {
        reference = seen;
      }
    }
  }
}

TEST_CASE("a post carried from the drain whose barrier is its start is handed by its arrival")
{
  for(auto const* entry : {"alu_entry", "idle_forever"})
  {
    for(auto const& how : variants())
    {
      CAPTURE(entry);
      CAPTURE(how.name);
      auto const run = scripted(how, entry, std::nullopt, 500 * us_ps, 1 * ms_ps);
      CHECK(run.reason == core::pause_reason::system_time);
      REQUIRE(run.exact);
      CHECK(run.late <= 8 * instruction_ps);
      if(std::string_view{entry} == "alu_entry" and how.mode == core::sync_mode::clocks)
      {
        CHECK(run.sender_ahead > 0);
      }
    }
  }
}

TEST_CASE("a requested pause in clock mode leaves every member at one time")
{
  for(auto const& how : clock_variants())
  {
    for(int repeat = 0; repeat < 5; ++repeat)
    {
      CAPTURE(how.name);
      CAPTURE(repeat);
      pinned const pin{how.one_cpu};
      rig          r{
          .members = {
                      member("alu", "alu_entry"), member("alu", "alu_entry"),
                      member("idle", "idle_forever")}
      };
      r.build(how);
      auto                           first  = r.goal(0, {.budget = icount{1'000'000'000'000}});
      auto                           second = r.goal(1, {.budget = icount{1'000'000'000'000}});
      core::host_future<core::pause> pending;
      {
        auto const held = r.sys->lock();
        pending         = r.sys->resume(held);
      }
      std::this_thread::sleep_for(std::chrono::milliseconds{2 + repeat});
      r.sys->request_pause();
      auto const stopped = pending.get();
      REQUIRE(stopped.reason == core::pause_reason::requested);
      auto const held = r.sys->lock();
      for(std::size_t index = 0; index < 3; ++index)
      {
        CAPTURE(index);
        CHECK(r.sys->time_of(held, index) == stopped.at);
      }
      CHECK(r.sys->pending_barrier(held) == stopped.at);
    }
  }
}

namespace
{
class bare_link final : public core::member_link
{
public:
  bare_link(std::size_t from, std::size_t to, std::int64_t latency)
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

  void enter_slice(core::time_map) override
  {}

  auto posted_count() const noexcept -> std::uint64_t override
  {
    return 0;
  }

  void sender_reset(core::sys_time) override
  {}

  void receiver_reset(core::sys_time) override
  {}

  auto drain(
      std::uint64_t,
      core::sys_time const&,
      core::sys_time const&,
      core::time_map,
      core::sys_time const&) -> std::optional<core::link_fault> override
  {
    return std::nullopt;
  }

  auto due() const -> std::optional<core::sys_time> override
  {
    return std::nullopt;
  }

  auto quiet() const -> bool override
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
  std::int64_t latency_;
};

class watched_link final : public core::member_link
{
public:
  watched_link(std::size_t from, std::size_t to, std::int64_t latency)
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

  void enter_slice(core::time_map) override
  {}

  auto posted_count() const noexcept -> std::uint64_t override
  {
    return 0;
  }

  void sender_reset(core::sys_time) override
  {}

  void receiver_reset(core::sys_time) override
  {}

  auto drain(
      std::uint64_t,
      core::sys_time const&,
      core::sys_time const&,
      core::time_map,
      core::sys_time const&) -> std::optional<core::link_fault> override
  {
    return std::nullopt;
  }

  auto due() const -> std::optional<core::sys_time> override
  {
    return std::nullopt;
  }

  auto quiet() const -> bool override
  {
    return true;
  }

  auto watch_posts(std::move_only_function<void(core::sys_time)>) -> bool override
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
  std::int64_t latency_;
};
}

#if defined(__SANITIZE_THREAD__)
constexpr std::int64_t lost_wake_rounds = 50'000;
constexpr auto         lost_wake_guard  = std::chrono::seconds{60};
#else
constexpr std::int64_t lost_wake_rounds = 1'000'000;
constexpr auto         lost_wake_guard  = std::chrono::seconds{15};
#endif

TEST_CASE("a million clock publish and wait rounds between two members lose no wake")
{
  constexpr std::int64_t        rounds  = lost_wake_rounds;
  constexpr std::int64_t        latency = 1 * us_ps;
  auto                          how     = clock_variants()[0];
  std::vector<core::member_ref> members{member("alu", "alu_entry"), member("alu", "alu_entry")};
  std::vector<std::shared_ptr<core::member_link>> wires{
      std::make_shared<bare_link>(0, 1, latency), std::make_shared<bare_link>(1, 0, latency)};
  auto const published = std::make_shared<std::array<std::atomic<std::int64_t>, 2>>();
  auto       engines   = std::make_shared<std::array<std::mt19937, 2>>(
      std::array<std::mt19937, 2>{std::mt19937{7}, std::mt19937{11}});
  core::system sys{
      members,
      std::move(wires),
      how.bounds,
      [published, engines](std::size_t index) {
        published->at(index).fetch_add(1, std::memory_order_relaxed);
        std::uniform_int_distribution<int> spin{0, 8};
        for(int left = spin(engines->at(index)); left > 0; --left)
        {
          std::this_thread::yield();
        }
      },
      {},
      how.mode,
      core::system::default_lookahead,
      core::sys_time{how.progress}};
  core::host_future<core::pause> pending;
  {
    auto const                  held = sys.lock();
    std::vector<core::sys_time> offsets(2);
    sys.start(held, offsets);
    pending = sys.resume(held, core::sys_time{rounds * latency * 13 / 10});
  }
  std::atomic<bool> landed{false};
  std::jthread      guard{[&sys, &landed](std::stop_token const& stopping) {
    auto const deadline = std::chrono::steady_clock::now() + lost_wake_guard;
    while(not stopping.stop_requested() and std::chrono::steady_clock::now() < deadline)
    {
      std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    if(not landed.load())
    {
      sys.close();
    }
  }};
  auto const        stopped = pending.get();
  landed.store(true);
  guard.request_stop();
  REQUIRE(stopped.reason == core::pause_reason::system_time);
  CHECK(published->at(0).load() + published->at(1).load() >= rounds);
  CHECK(stopped.windows > 0);
}

TEST_CASE("a drain fault past a lagging receiver's reach in clock mode pauses on it at its anchor")
{
  constexpr std::int64_t latency = 20 * us_ps;
  for(auto const& how : clock_variants())
  {
    for(int repeat = 0; repeat < 5; ++repeat)
    {
      CAPTURE(how.name);
      CAPTURE(repeat);
      auto const run =
          scripted(how, "alu_entry", std::nullopt, std::nullopt, 2 * ms_ps, 500 * us_ps);
      REQUIRE(run.reason == core::pause_reason::error);
      REQUIRE(run.fault);
      CHECK(*run.fault > run.fault_reach);
      CHECK(run.member == 1);
      CHECK(run.at.ps == *run.fault);
      REQUIRE(run.times.size() == 2);
      check_at(run.times[1], *run.fault + latency);
      check_at(run.times[0], *run.fault + latency + 3 * latency);
    }
  }
}

TEST_CASE(
    "a TIME pause with a halted member and goal holders at different positions matches barrier "
    "mode")
{
  auto const make = [] {
    return std::vector<core::member_ref>{
        member("idle", "idle_forever"), member("alu", "alu_entry"), member("idle", "idle_forever")};
  };
  std::int64_t cut{};
  std::int64_t past{};
  for(std::int64_t step = 0; step < 16 and past <= cut; ++step)
  {
    cut = 1 * ms_ps + step * instruction_ps;
    rig r{.members = make()};
    r.build(variants()[0]);
    r.release(2);
    REQUIRE(r.run(cut).reason == core::pause_reason::system_time);
    past = r.now_of(1);
  }
  REQUIRE(past > cut);
  std::optional<std::vector<std::int64_t>> reference;
  for(auto const& how : variants())
  {
    CAPTURE(how.name);
    pinned const pin{how.one_cpu};
    rig          r{.members = make()};
    r.build(how);
    r.release(2);
    core::host_future<core::pause> pending;
    {
      auto const held = r.sys->lock();
      pending         = r.sys->resume(held, core::sys_time{cut});
    }
    std::atomic<bool> landed{false};
    std::jthread      guard{[&r, &landed](std::stop_token const& stopping) {
      auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};
      while(not stopping.stop_requested() and std::chrono::steady_clock::now() < deadline)
      {
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
      }
      if(not landed.load())
      {
        r.sys->close();
      }
    }};
    auto const        stopped = pending.get();
    landed.store(true);
    guard.request_stop();
    REQUIRE(stopped.reason == core::pause_reason::system_time);
    std::vector<std::int64_t> times{r.now_of(0), r.now_of(1), r.now_of(2)};
    CHECK(times[0] == cut);
    CHECK(times[1] > cut);
    if(reference)
    {
      CHECK(times == *reference);
    }
    else
    {
      reference = times;
    }
  }
}

namespace
{
struct failing_alignment
{
  std::array<std::shared_ptr<core::host>, 4> hosts{};
  std::atomic<bool>                          a_done{false};
  std::atomic<bool>                          b_done{false};
  std::atomic<bool>                          h_waiting{false};
  std::atomic<bool>                          c_waiting{false};
  std::atomic<bool>                          requested{false};
  std::atomic<bool>                          h_failed{false};
  std::atomic<std::int64_t>                  failed_at{-1};
  int                                        h_phase{};
  int                                        c_phase{};

  void open_all() noexcept
  {
    requested.store(true);
    requested.notify_all();
    h_failed.store(true);
    h_failed.notify_all();
  }
};

}

TEST_CASE(
    "a halted member failing while a TIME pause aligns stops its halted receiver at its point")
{
  constexpr std::int64_t latency = 20 * us_ps;
  constexpr std::int64_t far     = 10 * latency;
  constexpr std::int64_t until   = 1 * ms_ps;
  constexpr std::int64_t gate_h  = until - 5 * latency;
  constexpr std::int64_t gate_c  = until - 6 * latency;
  for(std::uint64_t const slice : {std::uint64_t{8}, std::uint64_t{3}})
  {
    for(int repeat = 0; repeat < 5; ++repeat)
    {
      CAPTURE(slice);
      CAPTURE(repeat);
      std::vector<core::member_ref> members{
          member("idle", "idle_forever"), member("alu", "alu_entry"),
          member("idle", "idle_forever"), member("idle", "idle_forever")};
      auto state = std::make_shared<failing_alignment>();
      for(std::size_t index = 0; index < members.size(); ++index)
      {
        state->hosts.at(index) = members[index].on;
      }
      std::vector<std::shared_ptr<core::member_link>> wires{
          std::make_shared<bare_link>(2, 3, latency), std::make_shared<bare_link>(3, 2, far),
          std::make_shared<bare_link>(2, 0, far),     std::make_shared<bare_link>(2, 1, far),
          std::make_shared<bare_link>(3, 0, far),     std::make_shared<bare_link>(3, 1, far)};
      core::system sys{
          members,
          std::move(wires),
          core::slice_bounds{
                             .max_instructions = icount{slice}, .wall_budget = std::chrono::milliseconds{1}},
          [state](std::size_t index) {
            auto const now = static_cast<std::int64_t>(
                state->hosts.at(index)->member_inline(core::member_id{}).virtual_clock().time().ps);
            switch(index)
            {
              case 0:
                state->a_done = state->a_done or now >= until;
                break;
              case 1:
                state->b_done = state->b_done or now >= until;
                break;
              case 2:
                if(state->h_phase == 0 and now >= gate_h)
                {
                  state->h_phase   = 1;
                  state->h_waiting = true;
                  state->requested.wait(false);
                }
                else if(state->h_phase == 1)
                {
                  state->h_phase = 2;
                }
                else if(state->h_phase == 2)
                {
                  state->h_phase   = 3;
                  state->failed_at = now;
                  state->h_failed  = true;
                  state->h_failed.notify_all();
                  throw std::runtime_error("halted member fails while aligning");
                }
                break;
              default:
                if(state->c_phase == 0 and now >= gate_c)
                {
                  state->c_phase   = 1;
                  state->c_waiting = true;
                  state->requested.wait(false);
                }
                else if(state->c_phase == 1)
                {
                  state->c_phase = 2;
                }
                else if(state->c_phase == 2)
                {
                  state->c_phase = 3;
                  state->h_failed.wait(false);
                }
                break;
            }
                             },
          {},
          core::sync_mode::clocks
      };
      at_scope_end const             opened{[state] { state->open_all(); }};
      core::host_future<core::pause> pending;
      {
        auto const                  held = sys.lock();
        std::vector<core::sys_time> offsets(members.size());
        sys.start(held, offsets);
        sys.release(held, 2, true);
        sys.release(held, 3, true);
        pending = sys.resume(held, core::sys_time{until});
      }
      std::atomic<bool> landed{false};
      std::jthread      guard{[&sys, &landed, state](std::stop_token const& stopping) {
        auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};
        while(not stopping.stop_requested() and std::chrono::steady_clock::now() < deadline)
        {
          std::this_thread::sleep_for(std::chrono::milliseconds{5});
        }
        if(not landed.load())
        {
          state->open_all();
          sys.close();
        }
      }};
      while(not(state->a_done and state->b_done and state->h_waiting and state->c_waiting)
            and not pending.done())
      {
        std::this_thread::sleep_for(std::chrono::microseconds{200});
      }
      sys.request_pause();
      state->requested = true;
      state->requested.notify_all();
      auto const stopped = pending.get();
      landed.store(true);
      guard.request_stop();
      REQUIRE(stopped.reason == core::pause_reason::error);
      CHECK(stopped.member == 2);
      auto const x = state->failed_at.load();
      REQUIRE(x >= gate_h);
      REQUIRE(x + latency < until);
      CHECK(stopped.at.ps == x);
      auto const held = sys.lock();
      CHECK(sys.time_of(held, 0).ps == until);
      CHECK(sys.time_of(held, 1).ps > until);
      CHECK(sys.time_of(held, 2).ps == x);
      CHECK(sys.time_of(held, 3).ps == x + latency);
    }
  }
}

namespace
{
struct held_fault_rig
{
  std::array<std::shared_ptr<core::host>, 5> hosts{};
  std::atomic<bool>                          a_done{false};
  std::atomic<bool>                          b_done{false};
  std::atomic<int>                           waiting{0};
  std::atomic<bool>                          requested{false};
  std::atomic<bool>                          armed{false};
  std::atomic<bool>                          found{false};
  std::atomic<std::int64_t>                  fault_at{-1};
  std::array<int, 5>                         phases{};

  void open_all() noexcept
  {
    open_gate(requested);
    open_gate(found);
  }
};

class faulting_link final : public core::member_link
{
public:
  faulting_link(
      std::size_t                     from,
      std::size_t                     to,
      std::int64_t                    latency,
      std::shared_ptr<held_fault_rig> state)
      : from_{from}, to_{to}, latency_{latency}, state_{std::move(state)}
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

  void enter_slice(core::time_map) override
  {}

  auto posted_count() const noexcept -> std::uint64_t override
  {
    return 0;
  }

  void sender_reset(core::sys_time) override
  {}

  void receiver_reset(core::sys_time) override
  {}

  auto drain(
      std::uint64_t,
      core::sys_time const& barrier,
      core::sys_time const&,
      core::time_map,
      core::sys_time const&) -> std::optional<core::link_fault> override
  {
    if(not state_->armed.exchange(false))
    {
      return std::nullopt;
    }
    state_->fault_at = barrier.ps;
    open_gate(state_->found);
    return core::link_fault{
        .at     = barrier,
        .member = from_,
        .error  = std::make_exception_ptr(test_failure("held fault"))};
  }

  auto due() const -> std::optional<core::sys_time> override
  {
    return std::nullopt;
  }

  auto quiet() const -> bool override
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
  std::size_t                     from_;
  std::size_t                     to_;
  std::int64_t                    latency_;
  std::shared_ptr<held_fault_rig> state_;
};
}

enum class pause_leg : std::uint8_t
{
  timed,
  requested,
  open,
};

TEST_CASE("a drain fault found while a pause aligns stops a lagging member at its point")
{
  constexpr std::int64_t latency = 20 * us_ps;
  constexpr std::int64_t onward  = 30 * us_ps;
  constexpr std::int64_t far     = 10 * latency;
  constexpr std::int64_t until   = 1 * ms_ps;
  constexpr std::int64_t gate_sr = until - 5 * latency;
  constexpr std::int64_t gate_c  = until - 8 * latency;
  constexpr std::size_t  s       = 2;
  constexpr std::size_t  r       = 3;
  constexpr std::size_t  c       = 4;
  auto const ticking = [](popcorn::testing::machine_fixture<popcorn::testing::test_arch>& f) {
    for(std::uint64_t tick = 1; tick <= 1'000; ++tick)
    {
      std::ignore = f.module->attached_events().schedule(icount{tick * 8}, logged_kind, 0);
    }
  };
  for(auto const leg : {pause_leg::timed, pause_leg::requested, pause_leg::open})
  {
    auto const timed = leg == pause_leg::timed;
    for(std::uint64_t const slice : {std::uint64_t{8}, std::uint64_t{3}})
    {
      for(int repeat = 0; repeat < 5; ++repeat)
      {
        CAPTURE(static_cast<int>(leg));
        CAPTURE(slice);
        CAPTURE(repeat);
        auto const halted = [&] {
          return leg == pause_leg::open
                   ? member<popcorn::testing::test_arch>("idle", "idle_forever", ticking)
                   : member("idle", "idle_forever");
        };
        std::vector<core::member_ref> members{
            member("idle", "idle_forever"), member("alu", "alu_entry"), halted(), halted(),
            halted()};
        auto state = std::make_shared<held_fault_rig>();
        for(std::size_t index = 0; index < members.size(); ++index)
        {
          state->hosts.at(index) = members[index].on;
        }
        std::vector<std::shared_ptr<core::member_link>> wires{
            std::make_shared<faulting_link>(s, r, latency, state),
            std::make_shared<bare_link>(r, c, onward)};
        for(std::size_t from : {s, r, c})
        {
          for(std::size_t to : {std::size_t{0}, std::size_t{1}})
          {
            wires.push_back(std::make_shared<bare_link>(from, to, far));
          }
        }
        wires.push_back(std::make_shared<bare_link>(r, s, far));
        wires.push_back(std::make_shared<bare_link>(c, s, far));
        wires.push_back(std::make_shared<bare_link>(c, r, far));
        core::system sys{
            members,
            std::move(wires),
            core::slice_bounds{
                               .max_instructions = icount{slice}, .wall_budget = std::chrono::milliseconds{1}},
            [state](std::size_t index) {
              auto const now   = static_cast<std::int64_t>(state->hosts.at(index)
                                                               ->member_inline(core::member_id{})
                                                               .virtual_clock()
                                                               .time()
                                                               .ps);
              auto&      phase = state->phases.at(index);
              if(index == 0)
              {
                state->a_done = state->a_done or now >= until;
                return;
              }
              if(index == 1)
              {
                state->b_done = state->b_done or now >= until;
                return;
              }
              if(phase == 0 and now >= (index == c ? gate_c : gate_sr))
              {
                phase = 1;
                ++state->waiting;
                state->requested.wait(false);
              }
              else if(phase == 1)
              {
                phase = 2;
              }
              else if(phase == 2 and index == r)
              {
                phase        = 3;
                state->armed = true;
              }
              else if(phase == 2 and index == c)
              {
                phase = 3;
                state->found.wait(false);
              }
                               },
            {},
            core::sync_mode::clocks
        };
        at_scope_end const             opened{[state] { state->open_all(); }};
        core::host_future<core::pause> pending;
        {
          auto const                  held = sys.lock();
          std::vector<core::sys_time> offsets(members.size());
          sys.start(held, offsets);
          for(auto const halted : {s, r, c})
          {
            sys.release(held, halted, true);
          }
          if(leg == pause_leg::open)
          {
            for(std::size_t const holder : {std::size_t{0}, std::size_t{1}})
            {
              std::ignore = sys.post_goal(
                  held, holder, {.until = popcorn::address{0x0800'0001}, .open = true});
            }
            pending = sys.resume(held);
          }
          else
          {
            pending = sys.resume(held, core::sys_time{timed ? until : 10 * until});
          }
        }
        std::atomic<bool> landed{false};
        std::jthread      guard{[&sys, &landed, state](std::stop_token const& stopping) {
          auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};
          while(not stopping.stop_requested() and std::chrono::steady_clock::now() < deadline)
          {
            std::this_thread::sleep_for(std::chrono::milliseconds{5});
          }
          if(not landed.load())
          {
            state->open_all();
            sys.close();
          }
        }};
        while(not(state->waiting == 3 and (not timed or (state->a_done and state->b_done)))
              and not pending.done())
        {
          std::this_thread::sleep_for(std::chrono::microseconds{200});
        }
        sys.request_pause();
        open_gate(state->requested);
        auto const stopped = pending.get();
        landed.store(true);
        guard.request_stop();
        REQUIRE(stopped.reason == core::pause_reason::error);
        CHECK(stopped.member == s);
        auto const t = state->fault_at.load();
        REQUIRE(t > gate_c);
        CHECK(stopped.at.ps == t);
        auto const held = sys.lock();
        CHECK(sys.time_of(held, r).ps == t + latency);
        CHECK(sys.time_of(held, c).ps == t + latency + onward);
      }
    }
  }
}

TEST_CASE("a stamp dated inside a leading member's published lead is refused at its reach")
{
  for(auto const& how : clock_variants())
  {
    if(how.seed)
    {
      continue;
    }
    CAPTURE(how.name);
    pinned const pin{how.one_cpu};
    auto const   published = std::make_shared<std::atomic<int>>(0);
    rig          r{
        .members = {member("idle", "idle_forever"), member("alu", "alu_entry")},
        .arrival = [published](std::size_t index) {
          if(index == 1)
          {
            ++*published;
          }
                    }
    };
    r.build(how);
    REQUIRE(r.run(200 * us_ps).reason == core::pause_reason::system_time);
    REQUIRE(r.now_of(0) == 200 * us_ps);
    std::ignore = r.goal(0, {.until = popcorn::address{0x0800'0001}, .open = true});
    std::ignore = r.goal(1, {.until = popcorn::address{0x0800'0001}, .open = true});
    core::host_future<core::pause> pending;
    {
      auto const held = r.sys->lock();
      pending         = r.sys->resume(held);
    }
    while(published->load() < 200 and not pending.done())
    {
      std::this_thread::yield();
    }
    core::host_future<vtime> landed;
    {
      auto const held_host = r.members[0].on->lock();
      landed = r.members[0].on->post(held_host, 0, [](core::machine&) {}, vtime{200 * us_ps + 1});
    }
    CHECK_THROWS_WITH_AS(
        std::ignore = landed.get(),
        doctest::Contains("the earliest time an input lands on the member"), std::invalid_argument);
    r.sys->request_pause();
    CHECK(pending.get().reason == core::pause_reason::requested);
  }
}

TEST_CASE("an idle attempt hands frames at its earliest member, so a fault past it waits")
{
  for(auto const& how : {variants()[0], variants()[1], variants()[7], variants()[8]})
  {
    CAPTURE(how.name);
    auto const clocked = how.mode == core::sync_mode::clocks;
    rig        r{
        .members = {sleeper_with_event(400), sleeper_with_event(900)}
    };
    r.build(how);
    if(not clocked)
    {
      r.links[0]->fault_at = 222 * us_ps;
    }
    auto const count     = std::make_shared<std::atomic<std::size_t>>(0);
    r.links[0]->on_drain = [count](std::int64_t) { count->fetch_add(1); };
    auto a               = r.goal(0, {.until = popcorn::address{0x0800'0001}, .open = true});
    auto b               = r.goal(1, {.until = popcorn::address{0x0800'0001}, .open = true});
    core::host_future<core::pause> pending;
    {
      auto const held = r.sys->lock();
      pending         = r.sys->resume(held);
    }
    REQUIRE(both_asleep(r, pending));
    auto const before = count->load();
    r.sys->request_pause();
    CHECK(pending.get().reason != core::pause_reason::closed);
    auto const& drains = r.links[0]->drains;
    REQUIRE(drains.size() >= before);
    auto const confirms = std::ranges::count_if(
        drains | std::views::take(before), [](auto const& one) { return one.barrier == one.next; });
    CHECK(confirms >= 1);
    CHECK(std::ranges::all_of(drains | std::views::take(before), [](auto const& one) {
      return one.barrier != one.next or one.barrier <= 400 * instruction_ps;
    }));
  }
}

TEST_CASE("a quiet member moves by the lookahead and lands a stamp at its position plus it")
{
  constexpr std::int64_t lookahead = core::system::default_lookahead.ps;
  constexpr std::int64_t start     = 200 * us_ps;
  constexpr std::int64_t gate      = 30 * ms_ps;
  for(auto const& how : {variants()[7], variants()[10]})
  {
    CAPTURE(how.name);
    pinned const pin{how.one_cpu};
    struct shared
    {
      std::array<std::shared_ptr<core::host>, 2> hosts{};
      std::atomic<std::int64_t>                  quiet_at{0};
      std::mutex                                 mutex;
      std::vector<std::int64_t>                  moves;
      std::atomic<bool>                          released{false};
    };
    auto state = std::make_shared<shared>();
    rig  r{
        .members = {member("idle", "idle_forever"), member("alu", "alu_entry")},
        .arrival = [state](std::size_t index) {
          auto const now = static_cast<std::int64_t>(
              state->hosts.at(index)->member_inline(core::member_id{}).virtual_clock().time().ps);
          if(index == 0)
          {
            if(now > state->quiet_at.load())
            {
              std::scoped_lock const lock{state->mutex};
              state->moves.push_back(now);
            }
            state->quiet_at = now;
          }
          else if(now >= gate)
          {
            state->released.wait(false);
          }
                    }
    };
    state->hosts = {r.members[0].on, r.members[1].on};
    r.build(how);
    REQUIRE(r.run(start).reason == core::pause_reason::system_time);
    at_scope_end const opened{[state] { open_gate(state->released); }};
    std::ignore     = r.goal(0, {.until = popcorn::address{0x0800'0001}, .open = true});
    std::ignore     = r.goal(1, {.until = popcorn::address{0x0800'0001}, .open = true});
    state->quiet_at = start;
    {
      std::scoped_lock const lock{state->mutex};
      state->moves.clear();
    }
    core::host_future<core::pause> pending;
    {
      auto const held = r.sys->lock();
      pending         = r.sys->resume(held);
    }
    auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};
    while(state->quiet_at.load() < start + 2 * lookahead
          and not pending.done()
          and std::chrono::steady_clock::now() < deadline)
    {
      std::this_thread::yield();
    }
    auto const quiet_at = state->quiet_at.load();
    if(quiet_at != start + 2 * lookahead)
    {
      open_gate(state->released);
      r.sys->close();
    }
    REQUIRE(quiet_at == start + 2 * lookahead);
    core::host_future<vtime> landed;
    {
      auto const held_host = r.members[0].on->lock();
      landed               = r.members[0].on->post(
          held_host, 0, [](core::machine&) {},
          vtime{static_cast<std::uint64_t>(quiet_at + lookahead)});
    }
    open_gate(state->released);
    CHECK(landed.get().ps == static_cast<std::uint64_t>(quiet_at + lookahead));
    r.sys->request_pause();
    CHECK(pending.get().reason == core::pause_reason::requested);
    std::scoped_lock const lock{state->mutex};
    CHECK(state->moves.size() >= 2);
    CHECK(state->moves.at(0) == start + lookahead);
    CHECK(state->moves.at(1) == start + 2 * lookahead);
  }
}

TEST_CASE("a lookahead below a link's latency still lets a member reach its pause point")
{
  constexpr std::int64_t        latency   = 20 * us_ps;
  constexpr std::int64_t        lookahead = 5 * us_ps;
  std::vector<core::member_ref> members{member("alu", "alu_entry"), member("alu", "alu_entry")};
  auto const                    hosts  = std::array{members[0].on, members[1].on};
  auto const                    failed = std::make_shared<std::atomic<std::int64_t>>(-1);
  core::system                  sys{
      members,
      {std::make_shared<bare_link>(0, 1, latency)},
      {},
      [hosts, failed](std::size_t index) {
        auto const now = static_cast<std::int64_t>(
            hosts.at(index)->member_inline(core::member_id{}).virtual_clock().time().ps);
        if(index == 0 and now >= 100 * us_ps and failed->load() < 0)
        {
          failed->store(now);
          throw std::runtime_error("member 0 fails");
        }
      },
      {},
      core::sync_mode::clocks,
      core::sys_time{lookahead}};
  core::host_future<core::pause> pending;
  {
    auto const                  held = sys.lock();
    std::vector<core::sys_time> offsets(2);
    sys.start(held, offsets);
    pending = sys.resume(held, core::sys_time{1 * ms_ps});
  }
  std::atomic<bool> landed{false};
  std::jthread      guard{[&sys, &landed](std::stop_token const& stopping) {
    auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};
    while(not stopping.stop_requested() and std::chrono::steady_clock::now() < deadline)
    {
      std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    if(not landed.load())
    {
      sys.close();
    }
  }};
  auto const        stopped = pending.get();
  landed.store(true);
  guard.request_stop();
  REQUIRE(stopped.reason == core::pause_reason::error);
  auto const held = sys.lock();
  CHECK(sys.time_of(held, 0).ps == failed->load());
  CHECK(sys.time_of(held, 1).ps >= failed->load() + latency);
}

namespace
{
struct owner_gate
{
  std::array<std::shared_ptr<core::host>, 3> hosts{};
  std::atomic<std::int64_t>                  held_at{-1};
  std::atomic<std::int64_t>                  shown_max{-1};
  std::atomic<std::int64_t>                  barrier_max{-1};
  std::atomic<bool>                          released{false};
};

struct clock_waits
{
  std::array<std::atomic<std::uint64_t>, 3>                     turns{};
  std::array<std::atomic<std::atomic<std::uint32_t> const*>, 3> epochs{};
  std::array<std::atomic<std::uint32_t>, 3>                     seen{};
};

clock_waits waits_noted;

void note_wait(std::size_t member, std::atomic<std::uint32_t> const* epoch, std::uint32_t seen)
{
  if(member >= waits_noted.turns.size())
  {
    return;
  }
  if(epoch != nullptr)
  {
    waits_noted.epochs.at(member).store(epoch);
    waits_noted.seen.at(member).store(seen);
  }
  waits_noted.turns.at(member).fetch_add(1);
}

void unhook_waits() noexcept
{
  core::detail::on_clock_wait.store(nullptr);
}

auto blocked(std::initializer_list<std::size_t> members) -> bool
{
  std::array<std::uint64_t, 3> before{};
  for(auto const member : members)
  {
    before.at(member)       = waits_noted.turns.at(member).load();
    auto const* const epoch = waits_noted.epochs.at(member).load();
    if(before.at(member) % 2 == 0
       or epoch == nullptr
       or epoch->load() != waits_noted.seen.at(member).load())
    {
      return false;
    }
  }
  return std::ranges::all_of(members, [&before](std::size_t member) {
    return waits_noted.turns.at(member).load() == before.at(member)
       and waits_noted.epochs.at(member).load()->load() == waits_noted.seen.at(member).load();
  });
}
}

TEST_CASE("a run drain hands and shows no frame past an off-link cut owner's clock")
{
  for(auto how : {variants()[7], variants()[10]})
  {
    CAPTURE(how.name);
    how.bounds = {.max_instructions = icount{8}, .wall_budget = std::chrono::milliseconds{1}};
    pinned const pin{how.one_cpu};
    for(std::size_t member = 0; member < waits_noted.turns.size(); ++member)
    {
      waits_noted.turns.at(member).store(0);
      waits_noted.epochs.at(member).store(nullptr);
    }
    core::detail::on_clock_wait.store(&note_wait);
    at_scope_end const unhooked{&unhook_waits};
    auto               state = std::make_shared<owner_gate>();
    rig                r{
        .members =
            {member("alu", "alu_entry"), member("alu", "alu_entry"), member("alu", "alu_entry")},
        .arrival = [state](std::size_t index) {
          if(index != 2 or state->released.load())
          {
            return;
          }
          auto const now = static_cast<std::int64_t>(
              state->hosts.at(2)->member_inline(core::member_id{}).virtual_clock().time().ps);
          if(now > 100 * us_ps and state->held_at.load() < 0)
          {
            state->held_at = now;
            state->released.wait(false);
          }
                      }
    };
    state->hosts = {r.members[0].on, r.members[1].on, r.members[2].on};
    r.build(how);
    at_scope_end const opened{[state] { open_gate(state->released); }};
    r.links[0]->on_shown = [state](std::int64_t barrier, std::int64_t, std::int64_t shown) {
      if(state->released.load())
      {
        return;
      }
      state->shown_max.store(std::max(state->shown_max.load(), shown));
      state->barrier_max.store(std::max(state->barrier_max.load(), barrier));
    };
    {
      auto const held = r.sys->lock();
      r.sys->set_cut_owner(held, 0, 2);
    }
    for(std::size_t member = 0; member < 3; ++member)
    {
      std::ignore = r.goal(member, {.until = popcorn::address{0x0800'0001}, .open = true});
    }
    core::host_future<core::pause> pending;
    {
      auto const held = r.sys->lock();
      pending         = r.sys->resume(held);
    }
    auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};
    auto const settled  = [&state] { return state->held_at.load() >= 0 and blocked({0, 1}); };
    while(not settled() and std::chrono::steady_clock::now() < deadline)
    {
      std::this_thread::yield();
    }
    REQUIRE(settled());
    CHECK(state->barrier_max.load() >= state->held_at.load() - 5 * us_ps);
    CHECK(state->barrier_max.load() <= state->held_at.load());
    CHECK(state->shown_max.load() <= state->held_at.load());
    open_gate(state->released);
    r.sys->request_pause();
    CHECK(pending.get().reason == core::pause_reason::requested);
  }
}

TEST_CASE("a confirm drain hands and shows no frame past an off-link cut owner's stop")
{
  for(auto const& how : {variants()[7], variants()[10]})
  {
    CAPTURE(how.name);
    pinned const pin{how.one_cpu};
    rig          r{
        .members = {
                    member("alu", "alu_entry"), member("alu", "alu_entry"), member("alu", "alu_entry")}
    };
    r.build(how);
    auto const confirms  = std::make_shared<std::vector<std::pair<std::int64_t, std::int64_t>>>();
    r.links[0]->on_shown = [confirms](std::int64_t barrier, std::int64_t next, std::int64_t shown) {
      if(barrier == next)
      {
        confirms->emplace_back(barrier, shown);
      }
    };
    {
      auto const held = r.sys->lock();
      r.sys->set_cut_owner(held, 0, 2);
    }
    std::ignore   = r.goal(0, {.until = popcorn::address{0x0800'0001}, .open = true});
    std::ignore   = r.goal(1, {.until = popcorn::address{0x0800'0001}, .open = true});
    auto stopping = r.goal(2, {.until_time = core::sys_time{100 * us_ps}});
    core::host_future<core::pause> pending;
    {
      auto const held = r.sys->lock();
      pending         = r.sys->resume(held);
    }
    auto const stopped = pending.get();
    REQUIRE(stopped.reason == core::pause_reason::delivered);
    REQUIRE(stopped.member == 2);
    REQUIRE_FALSE(confirms->empty());
    CHECK(confirms->back().first == stopped.at.ps);
    CHECK(confirms->back().second == stopped.at.ps);
  }
}

TEST_CASE("a TIME pause releases a link's tap through its ends' positions")
{
  for(auto const& [how, offset] : {
          std::pair{variants()[7],  0},
          std::pair{variants()[10], 0},
          std::pair{variants()[7],  1},
          std::pair{variants()[10], 1}
  })
  {
    CAPTURE(how.name);
    CAPTURE(offset);
    pinned const pin{how.one_cpu};
    rig          r{
        .members = {member("alu", "alu_entry"), member("alu", "alu_entry")}
    };
    std::vector<std::shared_ptr<core::member_link>> wires;
    r.links.push_back(std::make_shared<fake_link>(0, 1, 20 * us_ps));
    wires.push_back(r.links[0]);
    r.sys = std::make_unique<core::system>(
        r.members, std::move(wires), how.bounds, std::move_only_function<void(std::size_t)>{},
        std::move_only_function<void(std::size_t)>{}, how.mode, core::system::default_lookahead,
        core::sys_time{how.progress});
    {
      auto const                  held = r.sys->lock();
      std::vector<core::sys_time> offsets{core::sys_time{0}, core::sys_time{0}};
      offsets.at(1 - offset) = core::sys_time{100'000};
      r.sys->start(held, offsets);
    }
    auto const released    = std::make_shared<std::vector<std::int64_t>>();
    r.links[0]->on_release = [released](std::int64_t at) { released->push_back(at); };
    auto const stopped     = r.run(1 * ms_ps + 10'000);
    REQUIRE(stopped.reason == core::pause_reason::system_time);
    auto const first  = r.now_of(0);
    auto const second = r.now_of(1);
    REQUIRE(first != second);
    REQUIRE(released->size() == 1);
    CHECK(released->front() == std::min(first, second));
    CHECK(released->front() < std::max(first, second));
  }
}

TEST_CASE("a stamp between two slices of a sleep's move is refused at the settled clock")
{
  auto const how = variants()[8];
  struct shared
  {
    std::array<std::shared_ptr<core::host>, 2> hosts{};
    std::atomic<int>                           moves{0};
    std::atomic<std::int64_t>                  held_at{-1};
    std::atomic<bool>                          released{false};
  };
  auto state = std::make_shared<shared>();
  rig  r{
      .members = {sleeper_with_event(400), sleeper_with_event(20'000)},
      .arrival = [state](std::size_t index) {
        if(index != 0 or state->released.load())
        {
          return;
        }
        auto const now = static_cast<std::int64_t>(
            state->hosts.at(0)->member_inline(core::member_id{}).virtual_clock().time().ps);
        if(now > 1 * ms_ps)
        {
          if(++state->moves == 2)
          {
            state->held_at = now;
            state->released.wait(false);
          }
        }
                  }
  };
  state->hosts = {r.members[0].on, r.members[1].on};
  r.build(how);
  at_scope_end const opened{[state] { open_gate(state->released); }};
  std::ignore = r.goal(0, {.until = popcorn::address{0x0800'0001}, .open = true});
  std::ignore = r.goal(1, {.until = popcorn::address{0x0800'0001}, .open = true});
  core::host_future<core::pause> pending;
  {
    auto const held = r.sys->lock();
    pending         = r.sys->resume(held);
  }
  auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};
  while(state->held_at.load() < 0 and std::chrono::steady_clock::now() < deadline)
  {
    std::this_thread::yield();
  }
  auto const held_at = state->held_at.load();
  REQUIRE(held_at > 0);
  constexpr std::int64_t settled = 20'000 * instruction_ps - 20 * us_ps;
  REQUIRE(held_at < settled);
  auto stamp = [&r, at = (held_at + settled) / 2] {
    auto const held_host = r.members[0].on->lock();
    return r.members[0].on->post(
        held_host, 0, [](core::machine&) {}, vtime{static_cast<std::uint64_t>(at)});
  }();
  open_gate(state->released);
  CHECK_THROWS_WITH_AS(
      std::ignore = stamp.get(), doctest::Contains("before 4.980000 ms"), std::invalid_argument);
  r.sys->request_pause();
  CHECK(pending.get().reason != core::pause_reason::closed);
}

TEST_CASE("a wake that lands during a sleep's move keeps the member awake")
{
  enum class wake_by : std::uint8_t
  {
    pause,
    mail,
    stamp,
    pump,
  };
  for(auto const by : {wake_by::pause, wake_by::mail, wake_by::stamp, wake_by::pump})
  {
    CAPTURE(static_cast<int>(by));
    auto const how = variants()[8];
    struct shared
    {
      std::array<std::shared_ptr<core::host>, 2> hosts{};
      popcorn::testing::test_arch*               module{};
      std::atomic<int>                           moves{0};
      std::atomic<std::int64_t>                  held_at{-1};
      std::atomic<bool>                          released{false};
      std::atomic<bool>                          requested{false};
      std::atomic<bool>                          peer_arrived{false};
      std::atomic<bool>                          served{false};
      std::atomic<int>                           slept_after{-1};
      std::atomic<bool>                          armed{false};
      std::atomic<std::int64_t>                  pumped_at{-1};
    };
    auto state = std::make_shared<shared>();
    popcorn::testing::machine_fixture<popcorn::testing::test_arch> f{
        m33(), "idle", core::machine_options{}};
    f.set("pc", sym("idle", "idle_forever"));
    f.set("r0", 0);
    state->module = f.module;
    std::ignore   = f.module->attached_events().schedule(
        icount{f.target->virtual_clock().now().value + 400}, logged_kind, 7);
    std::vector<std::unique_ptr<core::machine>> machines;
    machines.push_back(std::move(f.target));
    auto const first = core::member_ref{
        .on = std::make_shared<core::host>(
            std::move(machines), core::slice_bounds{},
            core::host_hooks{
                .boundary =
                    [state](core::member_id) {
                      if(not state->armed.exchange(false))
                      {
                        return;
                      }
                      auto& m          = state->hosts.at(0)->member_inline(core::member_id{});
                      state->pumped_at = static_cast<std::int64_t>(m.virtual_clock().time().ps);
                      std::ignore      = state->module->attached_events().schedule(
                          icount{m.virtual_clock().now().value}, logged_kind, 9);
                      state->served = true;
                    }}),
        .name = "first"};
    rig r{
        .members = {first, sleeper_with_event(20'000)},
        .arrival =
            [state](std::size_t index) {
              if(index == 1)
              {
                if(state->requested.load())
                {
                  state->peer_arrived = true;
                }
                return;
              }
              if(state->released.load())
              {
                return;
              }
              auto const now = static_cast<std::int64_t>(
                  state->hosts.at(0)->member_inline(core::member_id{}).virtual_clock().time().ps);
              if(now > 1 * ms_ps and ++state->moves == 2)
              {
                state->held_at = now;
                state->released.wait(false);
              }
                    },
        .slept =
            [state](std::size_t index) {
              if(index != 0 or not state->served.load() or state->slept_after.load() >= 0)
              {
                return;
              }
              state->slept_after = std::ranges::contains(
                                       state->module->event_log, std::uint64_t{9},
                                       &popcorn::testing::test_arch::logged_event::payload)
                                     ? 1
                                     : 0;
                    }
    };
    state->hosts = {r.members[0].on, r.members[1].on};
    r.build(how);
    at_scope_end const opened{[state] { open_gate(state->released); }};
    std::ignore  = r.goal(0, {.until = popcorn::address{0x0800'0001}, .open = true});
    std::ignore  = r.goal(1, {.until = popcorn::address{0x0800'0001}, .open = true});
    auto pending = [&r] {
      auto const held = r.sys->lock();
      return r.sys->resume(held);
    }();
    auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};
    while(state->held_at.load() < 0 and std::chrono::steady_clock::now() < deadline)
    {
      std::this_thread::yield();
    }
    constexpr std::int64_t settled = 20'000 * instruction_ps - 20 * us_ps;
    REQUIRE(state->held_at.load() > 0);
    REQUIRE(state->held_at.load() < settled);
    auto posted = [&r, &state, by] {
      if(by == wake_by::pump)
      {
        state->armed = true;
        return core::host_future<vtime>{};
      }
      auto const held_host = r.members[0].on->lock();
      if(by == wake_by::stamp)
      {
        return r.members[0].on->post(
            held_host, 0, [](core::machine&) {},
            vtime{static_cast<std::uint64_t>(settled + 1 * us_ps)});
      }
      return r.members[0].on->post(held_host, 0, [state](core::machine& m) {
        std::ignore = state->module->attached_events().schedule(
            icount{m.virtual_clock().now().value}, logged_kind, 9);
        state->served = true;
      });
    }();
    if(by == wake_by::pause)
    {
      state->requested = true;
      r.sys->request_pause();
      while(not state->peer_arrived.load() and std::chrono::steady_clock::now() < deadline)
      {
        std::this_thread::yield();
      }
      REQUIRE(state->peer_arrived.load());
    }
    open_gate(state->released);
    if(by == wake_by::mail or by == wake_by::pump)
    {
      if(by == wake_by::mail)
      {
        REQUIRE(posted.wait_for(std::chrono::seconds{10}));
      }
      while(state->slept_after.load() < 0
            and std::chrono::steady_clock::now() < deadline + std::chrono::seconds{10})
      {
        std::this_thread::yield();
      }
      CHECK(state->slept_after.load() == 1);
      if(by == wake_by::pump)
      {
        CHECK(state->pumped_at.load() >= settled);
      }
    }
    if(by == wake_by::stamp)
    {
      CHECK(posted.wait_for(std::chrono::seconds{10}));
    }
    if(by != wake_by::pause)
    {
      r.sys->request_pause();
    }
    auto ended = pending.wait_for(std::chrono::seconds{10});
    CHECK(ended);
    if(not ended)
    {
      r.sys->wake();
    }
    CHECK(pending.get().reason == core::pause_reason::requested);
  }
}

TEST_CASE("a command on a leading member lands at its published clock")
{
  constexpr std::int64_t gate = 30 * ms_ps;
  for(auto const& [how, blocking] : {
          std::pair{variants()[7],  true },
          std::pair{variants()[10], true },
          std::pair{variants()[7],  false},
          std::pair{variants()[10], false}
  })
  {
    CAPTURE(how.name);
    CAPTURE(blocking);
    pinned const pin{how.one_cpu};
    struct shared
    {
      std::array<std::shared_ptr<core::host>, 2> hosts{};
      std::atomic<std::int64_t>                  runner_at{0};
      std::atomic<bool>                          released{false};
    };
    auto state = std::make_shared<shared>();
    rig  r{
        .members = {member("idle", "idle_forever"), member("alu", "alu_entry")},
        .arrival = [state, blocking](std::size_t index) {
          if(index != 1)
          {
            return;
          }
          auto const now = static_cast<std::int64_t>(
              state->hosts.at(1)->member_inline(core::member_id{}).virtual_clock().time().ps);
          state->runner_at = now;
          if(blocking and now >= gate)
          {
            state->released.wait(false);
          }
                    }
    };
    state->hosts = {r.members[0].on, r.members[1].on};
    r.build(how);
    at_scope_end const opened{[state] { open_gate(state->released); }};
    std::ignore = r.goal(0, {.until = popcorn::address{0x0800'0001}, .open = true});
    std::ignore = r.goal(1, {.until = popcorn::address{0x0800'0001}, .open = true});
    core::host_future<core::pause> pending;
    {
      auto const held = r.sys->lock();
      pending         = r.sys->resume(held);
    }
    auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};
    while(state->runner_at.load() < gate and std::chrono::steady_clock::now() < deadline)
    {
      std::this_thread::yield();
    }
    REQUIRE(state->runner_at.load() >= gate);
    std::this_thread::sleep_for(std::chrono::milliseconds{20});
    auto served = [&r] {
      auto const held_host = r.members[0].on->lock();
      return r.members[0].on->post(held_host, 0, [](core::machine&) {});
    }();
    REQUIRE(served.wait_for(std::chrono::seconds{10}));
    CHECK(served.get().ps >= static_cast<std::uint64_t>(gate - 20 * us_ps));
    open_gate(state->released);
    r.sys->request_pause();
    CHECK(pending.get().reason == core::pause_reason::requested);
  }
}

TEST_CASE("a boundary pump on a leading member lands its input at the member's reach")
{
  constexpr std::int64_t gate = 30 * ms_ps;
  for(auto const& how : {variants()[7], variants()[10]})
  {
    CAPTURE(how.name);
    pinned const pin{how.one_cpu};
    struct shared
    {
      popcorn::testing::test_arch* module{};
      std::shared_ptr<core::host>  runner{};
      std::atomic<std::int64_t>    runner_at{0};
      std::atomic<bool>            armed{false};
      std::atomic<std::int64_t>    pumped_at{-1};
      std::atomic<bool>            released{false};
      std::shared_ptr<core::host>  quiet{};
    };
    auto state = std::make_shared<shared>();
    popcorn::testing::machine_fixture<popcorn::testing::test_arch> f{
        m33(), "idle", core::machine_options{}};
    f.set("pc", sym("idle", "idle_forever"));
    f.set("r0", 0);
    state->module = f.module;
    std::vector<std::unique_ptr<core::machine>> machines;
    machines.push_back(std::move(f.target));
    state->quiet = std::make_shared<core::host>(
        std::move(machines), core::slice_bounds{},
        core::host_hooks{.boundary = [state](core::member_id) {
          if(not state->armed.exchange(false))
          {
            return;
          }
          auto& m          = state->quiet->member_inline(core::member_id{});
          state->pumped_at = static_cast<std::int64_t>(m.virtual_clock().time().ps);
          std::ignore      = state->module->attached_events().schedule(
              icount{m.virtual_clock().now().value}, logged_kind, 9);
        }});
    rig r{
        .members =
            {core::member_ref{.on = state->quiet, .name = "quiet"}, member("alu", "alu_entry")},
        .arrival = [state](std::size_t index) {
          if(index != 1)
          {
            return;
          }
          auto const now = static_cast<std::int64_t>(
              state->runner->member_inline(core::member_id{}).virtual_clock().time().ps);
          state->runner_at = now;
          if(now >= gate)
          {
            state->released.wait(false);
          }
                      }
    };
    state->runner = r.members[1].on;
    r.build(how);
    at_scope_end const opened{[state] { open_gate(state->released); }};
    std::ignore = r.goal(0, {.until = popcorn::address{0x0800'0001}, .open = true});
    std::ignore = r.goal(1, {.until = popcorn::address{0x0800'0001}, .open = true});
    core::host_future<core::pause> pending;
    {
      auto const held = r.sys->lock();
      pending         = r.sys->resume(held);
    }
    auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};
    while(state->runner_at.load() < 5 * ms_ps and std::chrono::steady_clock::now() < deadline)
    {
      std::this_thread::yield();
    }
    REQUIRE(state->runner_at.load() >= 5 * ms_ps);
    state->armed = true;
    while(state->pumped_at.load() < 0 and std::chrono::steady_clock::now() < deadline)
    {
      std::this_thread::yield();
    }
    REQUIRE(state->pumped_at.load() >= 0);
    CHECK(state->pumped_at.load() >= 5 * ms_ps);
    open_gate(state->released);
    r.sys->request_pause();
    CHECK(pending.get().reason == core::pause_reason::requested);
  }
}

namespace
{
class sleeping_arch : public popcorn::testing::test_arch
{
public:
  using test_arch::test_arch;

  [[nodiscard]] auto wake_pending() const -> bool override
  {
    return false;
  }
};

class quietly_failing_arch final : public sleeping_arch
{
public:
  using sleeping_arch::sleeping_arch;

  static constexpr std::uint32_t fail_event = 9;

  [[nodiscard]] auto event_pends(std::uint32_t kind, std::uint64_t) const -> bool override
  {
    return kind != fail_event;
  }

  void on_event(std::uint32_t kind, std::uint64_t payload) override
  {
    if(kind == fail_event)
    {
      throw test_failure("member event inside a promise");
    }
    sleeping_arch::on_event(kind, payload);
  }
};

class quiet_event_arch final : public sleeping_arch
{
public:
  using sleeping_arch::sleeping_arch;

  static constexpr std::uint32_t quiet_event = 11;

  [[nodiscard]] auto event_pends(std::uint32_t kind, std::uint64_t) const -> bool override
  {
    return kind != quiet_event;
  }

  void on_event(std::uint32_t kind, std::uint64_t payload) override
  {
    if(kind == quiet_event)
    {
      std::ignore = attached_events().schedule(
          icount{attached_clock().now().value + 400}, quiet_event, payload);
      return;
    }
    sleeping_arch::on_event(kind, payload);
  }
};

template <typename Arch> void sends_nothing(popcorn::testing::machine_fixture<Arch>& f)
{
  f.target->set_transmit_sources({{}});
}

auto promised_windows(core::sync_mode mode) -> std::pair<core::pause, std::array<std::int64_t, 2>>
{
  constexpr std::int64_t        latency = 20 * us_ps;
  std::vector<core::member_ref> members{
      member<popcorn::testing::test_arch>(
          "alu", "alu_entry", sends_nothing<popcorn::testing::test_arch>),
      member<sleeping_arch>("idle", "idle_entry", sends_nothing<sleeping_arch>)};
  core::system sys{
      members,
      {std::make_shared<watched_link>(0, 1, latency),
        std::make_shared<watched_link>(1, 0, latency)},
      core::slice_bounds{
        .max_instructions = icount{1'000'000}, .wall_budget = std::chrono::hours{1}},
      {},
      {},
      mode
  };
  core::host_future<core::pause> pending;
  {
    auto const                  held = sys.lock();
    std::vector<core::sys_time> offsets(2);
    sys.start(held, offsets);
    pending = sys.resume(held, core::sys_time{10 * ms_ps});
  }
  auto const stopped = pending.get();
  auto const held    = sys.lock();
  return {
      stopped, {sys.time_of(held, 0).ps, sys.time_of(held, 1).ps}
  };
}
}

TEST_CASE("a sleeping member's promise lets an awake peer run to the sleeper's next wake")
{
  auto const [barrier, stood]  = promised_windows(core::sync_mode::barrier);
  auto const [clocks, aligned] = promised_windows(core::sync_mode::clocks);
  REQUIRE(clocks.reason == core::pause_reason::system_time);
  CHECK(aligned == stood);
  MESSAGE("windows barrier " << barrier.windows << " clocks " << clocks.windows);
  CHECK(clocks.windows <= 40);
}

TEST_CASE("a promise record is read whole while its writer publishes")
{
#if defined(__SANITIZE_THREAD__)
  constexpr std::int64_t rounds = 50'000;
#else
  constexpr std::int64_t rounds = 1'000'000;
#endif
  constexpr std::size_t          width = 16;
  core::detail::published_record record;
  record.shape(width, width, width);
  std::atomic<bool>            done{false};
  std::jthread                 writer{[&record, &done] {
    core::detail::promise_record next{
        .wires    = std::vector<std::int64_t>(width),
        .consumed = std::vector<std::int64_t>(width),
        .heard    = std::vector<std::int64_t>(width)};
    for(std::int64_t round = 1; round <= rounds; ++round)
    {
      next.floor   = core::sys_time{round};
      next.general = core::sys_time{round};
      std::ranges::fill(next.wires, round);
      std::ranges::fill(next.consumed, round);
      std::ranges::fill(next.heard, round);
      record.write(next);
    }
    done.store(true, std::memory_order_release);
  }};
  core::detail::promise_record seen;
  std::uint64_t                reads     = 0;
  std::uint64_t                torn      = 0;
  std::uint64_t                backwards = 0;
  std::int64_t                 last      = 0;
  auto const                   whole     = [&seen] {
    auto const at = seen.floor.ps;
    auto const is = [at](std::int64_t value) { return value == at; };
    return seen.general.ps == at
       and std::ranges::all_of(seen.wires, is)
       and std::ranges::all_of(seen.consumed, is)
       and std::ranges::all_of(seen.heard, is);
  };
  while(not done.load(std::memory_order_acquire))
  {
    record.read(seen);
    ++reads;
    torn += whole() ? 0 : 1;
    backwards += seen.floor.ps < last ? 1 : 0;
    last = seen.floor.ps;
  }
  writer.join();
  record.read(seen);
  CHECK(reads > 0);
  CHECK(torn == 0);
  CHECK(backwards == 0);
  CHECK(seen.floor.ps == rounds);
  CHECK(whole());
}

TEST_CASE("an error inside a sleeper's promise aligns every member past the latest promise")
{
  constexpr std::int64_t      latency   = 20 * us_ps;
  constexpr std::int64_t      lookahead = 200 * us_ps;
  constexpr std::int64_t      failing   = 1777 * 250'000;
  std::optional<std::int64_t> reference;
  for(std::uint32_t const seed : {0U, 1U, 2U, 3U, 4U})
  {
    CAPTURE(seed);
    std::vector<core::member_ref> members{
        member<popcorn::testing::test_arch>(
            "alu", "alu_entry", sends_nothing<popcorn::testing::test_arch>),
        member<quietly_failing_arch>(
            "idle", "idle_forever", [](popcorn::testing::machine_fixture<quietly_failing_arch>& f) {
              f.target->set_transmit_sources({{}});
              std::ignore = f.module->attached_events().schedule(
                  icount{1777}, quietly_failing_arch::fail_event, 0);
            })};
    auto engines = std::make_shared<std::array<std::mt19937, 2>>(
        std::array{std::mt19937{seed}, std::mt19937{seed + 7}});
    core::system sys{
        members,
        {std::make_shared<watched_link>(0, 1, latency),
          std::make_shared<watched_link>(1, 0, latency)},
        core::slice_bounds{.max_instructions = icount{97}, .wall_budget = std::chrono::hours{1}},
        [engines](std::size_t index) {
          std::uniform_int_distribution<int> pause{0, 30};
          std::this_thread::sleep_for(std::chrono::microseconds{pause(engines->at(index))});
          },
        {},
        core::sync_mode::clocks,
        core::sys_time{lookahead}
    };
    core::host_future<core::pause> pending;
    {
      auto const                  held = sys.lock();
      std::vector<core::sys_time> offsets(2);
      sys.start(held, offsets);
      pending = sys.resume(held, core::sys_time{2 * ms_ps});
    }
    auto const stopped = pending.get();
    REQUIRE(stopped.reason == core::pause_reason::error);
    CHECK(stopped.member == 1);
    CHECK(stopped.at.ps == failing);
    auto const held = sys.lock();
    CHECK(sys.time_of(held, 1).ps == failing);
    auto const aligned = sys.time_of(held, 0).ps;
    CHECK(aligned >= failing + lookahead + latency);
    CHECK(aligned < failing + lookahead + latency + us_ps);
    if(reference)
    {
      CHECK(aligned == *reference);
    }
    reference = aligned;
  }
}

namespace
{
std::atomic<bool> sleeper_waits{false};
std::atomic<bool> sleeper_commanded{false};

void hold_sleeper(std::size_t member, std::atomic<std::uint32_t> const* epoch, std::uint32_t)
{
  if(member != 1 or epoch == nullptr or sleeper_commanded.load())
  {
    return;
  }
  sleeper_waits.store(true);
  while(not sleeper_commanded.load())
  {
    std::this_thread::yield();
  }
}
}

TEST_CASE("a command on a sleeper whose promise grows lands once its machine reaches it")
{
  constexpr std::int64_t        latency = 20 * us_ps;
  std::vector<core::member_ref> members{
      member<popcorn::testing::test_arch>(
          "alu", "alu_entry", sends_nothing<popcorn::testing::test_arch>),
      member<quiet_event_arch>(
          "idle", "idle_forever", [](popcorn::testing::machine_fixture<quiet_event_arch>& f) {
            f.target->set_transmit_sources({{}});
            std::ignore =
                f.module->attached_events().schedule(icount{400}, quiet_event_arch::quiet_event, 0);
          })};
  core::system sys{
      members,
      {std::make_shared<watched_link>(0, 1, latency),
        std::make_shared<watched_link>(1, 0, latency)},
      core::slice_bounds{.max_instructions = icount{1'000}, .wall_budget = std::chrono::hours{1}},
      {},
      {},
      core::sync_mode::clocks
  };
  core::host_future<core::pause> pending;
  {
    auto const                  held = sys.lock();
    std::vector<core::sys_time> offsets(2);
    sys.start(held, offsets);
    std::ignore = sys.post_goal(held, 0, {.until = popcorn::address{0x0800'0001}, .open = true});
    std::ignore = sys.post_goal(held, 1, {.until = popcorn::address{0x0800'0001}, .open = true});
    sleeper_waits.store(false);
    sleeper_commanded.store(false);
    core::detail::on_clock_wait.store(&hold_sleeper);
    pending = sys.resume(held);
  }
  while(not sleeper_waits.load())
  {
    std::this_thread::yield();
  }
  auto served = [&members] {
    auto const held_host = members[1].on->lock();
    return members[1].on->post(held_host, 0, [](core::machine&) {});
  }();
  sleeper_commanded.store(true);
  core::detail::on_clock_wait.store(nullptr);
  REQUIRE(served.wait_for(std::chrono::seconds{10}));
  std::ignore = served.get();
  sys.request_pause();
  std::ignore = pending.get();
}

namespace
{
class lines_quiet_arch final : public sleeping_arch
{
public:
  using sleeping_arch::sleeping_arch;

  [[nodiscard]] auto may_wake(std::uint32_t) const -> bool override
  {
    return false;
  }
};

class ram_poke_model final : public popcorn::periph::model
{
public:
  static constexpr popcorn::address poked{0x2000'0100};

  explicit ram_poke_model(popcorn::periph::model_context& ctx) noexcept : context_{ctx}
  {}

  void arm(vtime at)
  {
    std::ignore = context_.events.schedule_at(at, 0, 0, context_.clock);
  }

  [[nodiscard]] auto claims() const -> std::vector<std::string> override
  {
    return {};
  }

  void reset(popcorn::periph::register_file&, popcorn::reset_kind) override
  {}

  auto on_read(
      popcorn::periph::register_file&,
      std::uint32_t,
      std::uint8_t,
      popcorn::periph::access_ctx) -> std::expected<std::uint32_t, popcorn::bus_error> override
  {
    return 0u;
  }

  auto on_write(
      popcorn::periph::register_file&,
      std::uint32_t,
      std::uint8_t,
      std::uint32_t,
      std::uint32_t,
      popcorn::periph::access_ctx) -> std::expected<void, popcorn::bus_error> override
  {
    return {};
  }

  void on_event(std::uint32_t, std::uint64_t) override
  {
    std::ignore = context_.services.write(poked, 4, 0x5a, popcorn::initiator::dma);
  }

  void save(std::vector<std::byte>&) const override
  {}

  void load(std::span<std::byte const>) override
  {}

  [[nodiscard]] auto stateless() const -> bool override
  {
    return true;
  }

  [[nodiscard]] auto role_in_horizon() const noexcept -> popcorn::periph::wake_role override
  {
    return popcorn::periph::wake_role::lines;
  }

private:
  popcorn::periph::model_context& context_;
};
}

TEST_CASE("a DMA write a watch stops on inside a sleeper's promise aligns its peer at its time")
{
  constexpr std::int64_t   latency = 20 * us_ps;
  constexpr std::int64_t   poke    = 100 * us_ps;
  auto                     part    = m33();
  popcorn::peripheral_desc unit;
  unit.name = "P0";
  unit.ip   = "PROBE";
  unit.base = 0x4003'0000;
  unit.size = 0x400;
  unit.irqs = {4};
  part.peripherals.push_back(std::move(unit));
  ram_poke_model*       poker{};
  core::machine_options options;
  options.models.emplace(
      "P0", [&poker](popcorn::periph::model_context& ctx, popcorn::peripheral_desc const&) {
        auto made = std::make_shared<ram_poke_model>(ctx);
        poker     = made.get();
        return std::shared_ptr<popcorn::periph::model>{made};
      });
  popcorn::testing::machine_fixture<lines_quiet_arch> f{
      std::move(part), "idle", std::move(options)};
  f.set("pc", sym("idle", "idle_forever"));
  f.set("r0", 0);
  f.target->set_transmit_sources({{}});
  REQUIRE(poker != nullptr);
  poker->arm(vtime{poke});
  auto const watch = f.target->add_watch(
      ram_poke_model::poked, 4, core::watch_kind::write,
      [](core::watch_hit const&) { return core::watch_action::stop; });
  std::vector<std::unique_ptr<core::machine>> machines;
  machines.push_back(std::move(f.target));
  std::vector<core::member_ref> members{
      member<popcorn::testing::test_arch>(
          "alu", "alu_entry", sends_nothing<popcorn::testing::test_arch>),
      {.on = std::make_shared<core::host>(std::move(machines)), .name = "poked"}
  };
  core::system sys{
      members,
      {std::make_shared<watched_link>(0, 1, latency),
        std::make_shared<watched_link>(1, 0, latency)},
      core::slice_bounds{
        .max_instructions = icount{1'000'000}, .wall_budget = std::chrono::hours{1}},
      {},
      {},
      core::sync_mode::clocks
  };
  core::host_future<core::pause> pending;
  {
    auto const                  held = sys.lock();
    std::vector<core::sys_time> offsets(2);
    sys.start(held, offsets);
    pending = sys.resume(held, core::sys_time{10 * ms_ps});
  }
  auto const stopped = pending.get();
  REQUIRE(stopped.reason == core::pause_reason::delivered);
  CHECK(stopped.member == 1);
  CHECK(stopped.at.ps == poke);
  auto const held = sys.lock();
  CHECK(sys.time_of(held, 1).ps == poke);
  CHECK(sys.time_of(held, 0).ps >= poke + latency);
  CHECK(sys.time_of(held, 0).ps < poke + latency + us_ps);
}

namespace
{
struct follow_state
{
  std::atomic<std::uint64_t> peer_slices{};
  std::atomic<bool>          peer_waiting{false};
  std::uint64_t              calls{};
  std::uint64_t              base{};
  std::atomic<int>           followed{};
};

follow_state following;

void note_peer_wait(std::size_t member, std::atomic<std::uint32_t> const* epoch, std::uint32_t)
{
  if(member == 1)
  {
    following.peer_waiting.store(epoch != nullptr);
  }
}

template <typename Done> auto await_for(Done done) -> bool
{
  auto const end = std::chrono::steady_clock::now() + std::chrono::seconds{10};
  while(not done())
  {
    if(std::chrono::steady_clock::now() >= end)
    {
      return false;
    }
    std::this_thread::yield();
  }
  return true;
}

void follow_first(std::size_t member, core::sys_time)
{
  if(member != 0 or following.followed.load() != 0)
  {
    return;
  }
  ++following.calls;
  if(following.calls == 1)
  {
    if(not await_for([] { return following.peer_waiting.load(); }))
    {
      following.followed.store(-1);
      return;
    }
    following.base = following.peer_slices.load();
    return;
  }
  if(following.calls == 2)
  {
    auto const moved = await_for([] { return following.peer_slices.load() > following.base; });
    following.followed.store(moved ? 1 : -1);
  }
}

auto followed_run(std::int64_t progress) -> std::pair<core::pause, std::array<std::int64_t, 2>>
{
  constexpr std::int64_t        latency = 20 * us_ps;
  std::vector<core::member_ref> members{
      member<popcorn::testing::test_arch>(
          "alu", "alu_entry", sends_nothing<popcorn::testing::test_arch>),
      member<sleeping_arch>("idle", "idle_entry", sends_nothing<sleeping_arch>)};
  core::system sys{
      members,
      {std::make_shared<watched_link>(0, 1, latency),
        std::make_shared<watched_link>(1, 0, latency)},
      core::slice_bounds{
        .max_instructions = icount{1'000'000}, .wall_budget = std::chrono::hours{1}},
      [](std::size_t index) {
        if(index == 0)
        {
          following.calls = 0;
        }
        else
        {
          following.peer_slices.fetch_add(1);
        }
        },
      {},
      core::sync_mode::clocks,
      core::system::default_lookahead,
      core::sys_time{progress}
  };
  core::host_future<core::pause> pending;
  {
    auto const                  held = sys.lock();
    std::vector<core::sys_time> offsets(2);
    sys.start(held, offsets);
    pending = sys.resume(held, core::sys_time{10 * ms_ps});
  }
  auto const stopped = pending.get();
  auto const held    = sys.lock();
  return {
      stopped, {sys.time_of(held, 0).ps, sys.time_of(held, 1).ps}
  };
}
}

TEST_CASE("a sleeper follows an awake peer's floor while the peer's slice still runs")
{
  for(std::int64_t const progress : {50 * us_ps, std::int64_t{0}})
  {
    CAPTURE(progress);
    following.peer_slices.store(0);
    following.peer_waiting.store(false);
    following.calls = 0;
    following.followed.store(0);
    core::detail::on_clock_wait.store(&note_peer_wait);
    core::detail::on_progress.store(&follow_first);
    auto const [stopped, stood] = followed_run(progress);
    core::detail::on_progress.store(nullptr);
    core::detail::on_clock_wait.store(nullptr);
    REQUIRE(stopped.reason == core::pause_reason::system_time);
    if(progress != 0)
    {
      CHECK(following.followed.load() == 1);
    }
    else
    {
      CHECK(following.followed.load() == 0);
    }
    CHECK(stood == promised_windows(core::sync_mode::barrier).second);
  }
}

namespace
{
class ticking_quiet_arch final : public sleeping_arch
{
public:
  using sleeping_arch::sleeping_arch;

  static constexpr std::uint32_t tick   = 12;
  static constexpr std::uint64_t period = 40;

  [[nodiscard]] auto event_pends(std::uint32_t kind, std::uint64_t) const -> bool override
  {
    return kind != tick;
  }

  void on_event(std::uint32_t kind, std::uint64_t payload) override
  {
    if(kind == tick)
    {
      std::ignore =
          attached_events().schedule(icount{attached_clock().now().value + period}, tick, payload);
      return;
    }
    sleeping_arch::on_event(kind, payload);
  }
};

std::atomic<std::uint64_t> first_slices{};

auto followed_sleeper(std::int64_t progress) -> int
{
  constexpr std::int64_t        latency = 200 * us_ps;
  std::vector<core::member_ref> members{
      member<ticking_quiet_arch>(
          "idle", "idle_forever",
          [](popcorn::testing::machine_fixture<ticking_quiet_arch>& f) {
            f.target->set_transmit_sources({{}});
            std::ignore = f.module->attached_events().schedule(
                icount{ticking_quiet_arch::period}, ticking_quiet_arch::tick, 0);
          }),
      member<popcorn::testing::test_arch>(
          "alu", "alu_entry", sends_nothing<popcorn::testing::test_arch>)};
  core::system sys{
      members,
      {std::make_shared<watched_link>(0, 1, latency),
        std::make_shared<watched_link>(1, 0, latency)},
      core::slice_bounds{
        .max_instructions = icount{1'000'000}, .wall_budget = std::chrono::hours{1}},
      [](std::size_t index) {
        if(index == 0)
        {
          following.calls = 0;
          first_slices.fetch_add(1);
        }
        else
        {
          following.peer_slices.fetch_add(1);
        }
        },
      {},
      core::sync_mode::clocks,
      core::system::default_lookahead,
      core::sys_time{progress}
  };
  core::host_future<core::pause> pending;
  {
    auto const                  held = sys.lock();
    std::vector<core::sys_time> offsets(2);
    sys.start(held, offsets);
    std::ignore = sys.post_goal(
        held, 0,
        {.until = popcorn::address{0x0800'0001}, .stop_when = [] { return false; }, .open = true});
    std::ignore = sys.post_goal(held, 1, {.until = popcorn::address{0x0800'0001}, .open = true});
    pending     = sys.resume(held);
  }
  auto const ended =
      await_for([] { return following.followed.load() != 0 or first_slices.load() >= 20; });
  sys.request_pause();
  std::ignore = pending.get();
  return ended ? following.followed.load() : -2;
}
}

TEST_CASE("an awake peer follows a sleeper with no promise while the sleeper's slice still runs")
{
  for(std::int64_t const progress : {25 * us_ps, std::int64_t{0}})
  {
    CAPTURE(progress);
    following.peer_slices.store(0);
    following.peer_waiting.store(false);
    following.calls = 0;
    following.followed.store(0);
    first_slices.store(0);
    core::detail::on_clock_wait.store(&note_peer_wait);
    core::detail::on_progress.store(&follow_first);
    auto const followed = followed_sleeper(progress);
    core::detail::on_progress.store(nullptr);
    core::detail::on_clock_wait.store(nullptr);
    CHECK(followed == (progress != 0 ? 1 : 0));
  }
}

TEST_CASE("a member reset mid-slice gives the same pause and machines at every progress")
{
  std::optional<std::pair<core::pause_reason, std::vector<std::uint64_t>>> reference;
  for(auto const& how : clock_variants())
  {
    CAPTURE(how.name);
    pinned const pin{how.one_cpu};
    auto         resetting = member("calls", "reset_now");
    resetting.on_reset     = popcorn::core::reset_request_policy::reset_and_continue;
    rig r{
        .members = {member("alu", "alu_entry"), resetting}
    };
    r.build(how);
    auto       goal    = r.goal(1, {.budget = icount{5000}});
    auto const stopped = r.run({});
    REQUIRE(stopped.reason == core::pause_reason::delivered);
    CHECK(goal.get().stopped.reason == stop_reason::budget);
    CHECK(r.links[0]->receiver_resets.size() == 1);
    std::pair seen{stopped.reason, r.dump()};
    if(reference)
    {
      CHECK(seen == *reference);
    }
    else
    {
      reference = std::move(seen);
    }
  }
}
