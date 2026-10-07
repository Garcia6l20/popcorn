#include <popcorn/core/errors.hpp>
#include <popcorn/core/host.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/core/trace.hpp>

#include "../support/machine_fixture.hpp"
#include "../support/test_arch.hpp"

#include <doctest/doctest.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/resource.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <exception>
#include <functional>
#include <latch>
#include <limits>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace
{
namespace core = popcorn::core;
using popcorn::address;
using popcorn::icount;
using popcorn::stop_reason;
using popcorn::vtime;
using popcorn::testing::m33;
using popcorn::testing::sym;
using namespace std::chrono_literals;

constexpr auto long_wall = std::chrono::seconds{60};

struct test_failure : std::runtime_error
{
  using std::runtime_error::runtime_error;
};

class stop_at_arch final : public popcorn::testing::test_arch
{
public:
  using test_arch::test_arch;

  static constexpr std::uint32_t stop_event = 7;

  void on_event(std::uint32_t kind, std::uint64_t payload) override
  {
    if(kind == stop_event)
    {
      attached_services().request_stop(stop_reason::user, "at");
      return;
    }
    test_arch::on_event(kind, payload);
  }
};

class ticking_arch final : public popcorn::testing::test_arch
{
public:
  using test_arch::test_arch;

  static constexpr std::uint32_t tick_event = 9;
  static constexpr std::uint64_t period     = 1'000;
  static constexpr address       counter_at = 0x2001'0000;

  std::uint64_t ticks = 0;

  void arm()
  {
    std::ignore = attached_events().schedule(icount{period}, tick_event, 0);
  }

  void on_event(std::uint32_t kind, std::uint64_t payload) override
  {
    if(kind == tick_event)
    {
      ++ticks;
      std::ignore = attached_services().write(
          counter_at, 4, static_cast<std::uint32_t>(ticks), popcorn::initiator::debugger);
      std::ignore =
          attached_events().schedule(icount{attached_clock().now().value + period}, tick_event, 0);
      return;
    }
    test_arch::on_event(kind, payload);
  }
};

class recording_arch final : public popcorn::testing::test_arch
{
public:
  recording_arch(popcorn::core_desc const& core, std::atomic<std::thread::id>* destroyed_on)
      : test_arch{core}, destroyed_on_{destroyed_on}
  {}

  recording_arch(recording_arch const&)                    = delete;
  auto operator=(recording_arch const&) -> recording_arch& = delete;

  ~recording_arch() override
  {
    destroyed_on_->store(std::this_thread::get_id());
  }

private:
  std::atomic<std::thread::id>* destroyed_on_;
};

class poking_arch final : public popcorn::testing::test_arch
{
public:
  using test_arch::test_arch;

  core::machine* poke = nullptr;

  auto on_interrupt(std::uint32_t intno) -> popcorn::arch::trap_result override
  {
    if(poke != nullptr)
    {
      std::exchange(poke, nullptr)->interrupt();
    }
    return test_arch::on_interrupt(intno);
  }
};

class resetting_arch final : public popcorn::testing::test_arch
{
public:
  using test_arch::test_arch;

  static constexpr std::uint32_t reset_event = 8;

  void on_event(std::uint32_t kind, std::uint64_t payload) override
  {
    if(kind == reset_event)
    {
      attached_services().request_reset(popcorn::reset_kind::software);
      return;
    }
    test_arch::on_event(kind, payload);
  }
};

constexpr std::uint64_t ps_per_instruction = 250'000;

auto at_instruction(std::uint64_t count) -> vtime
{
  return vtime{count * ps_per_instruction};
}

auto sram(core::machine& m) -> std::vector<std::byte>
{
  std::vector<std::byte> bytes(786'432);
  REQUIRE(m.mem().read_block(0x2000'0000, bytes).has_value());
  return bytes;
}

auto alu_entry_fixture(std::uint32_t loops) -> popcorn::testing::machine_fixture<>
{
  popcorn::testing::machine_fixture<> f{m33(), "alu", core::machine_options{}};
  f.set("r0", loops);
  return f;
}

auto alu(std::uint32_t loops) -> std::unique_ptr<core::machine>
{
  return std::move(alu_entry_fixture(loops).target);
}

auto idle_forever() -> std::unique_ptr<core::machine>
{
  popcorn::testing::machine_fixture<> f{m33(), "idle", core::machine_options{}};
  REQUIRE(f.run_to_stop().reason == stop_reason::user);
  f.set("pc", sym("idle", "idle_forever"));
  return std::move(f.target);
}

auto one(std::unique_ptr<core::machine> member) -> std::vector<std::unique_ptr<core::machine>>
{
  std::vector<std::unique_ptr<core::machine>> members;
  members.push_back(std::move(member));
  return members;
}

struct hosted
{
  std::vector<core::hook_handle> hooks{};
  std::unique_ptr<core::host>    on{};

  explicit hosted(
      std::unique_ptr<core::machine> member,
      core::slice_bounds             bounds = {},
      core::host_hooks               calls  = {})
  {
    adopt(std::move(member), bounds, std::move(calls));
  }

  hosted() = default;

  void adopt(
      std::unique_ptr<core::machine> member,
      core::slice_bounds             bounds = {},
      core::host_hooks               calls  = {})
  {
    on = std::make_unique<core::host>(one(std::move(member)), bounds, std::move(calls));
  }

  auto run(core::run_goal goal) const -> core::host_future<core::goal_result>
  {
    auto held = on->lock();
    return on->run(held, 0, std::move(goal));
  }

  void start() const
  {
    auto held = on->lock();
    on->start(held, 0);
  }

  auto wait(core::waiter_spec spec) const -> core::waiter_handle
  {
    auto held = on->lock();
    return on->wait(held, 0, std::move(spec));
  }

  auto post(std::move_only_function<void(core::machine&)> body, std::optional<vtime> at = {}) const
      -> core::host_future<vtime>
  {
    auto held = on->lock();
    return on->post(held, 0, std::move(body), at);
  }

  auto reg(std::string_view name) const -> std::uint64_t
  {
    return core::access(
        *on, 0, [name](core::machine& m) { return m.regs().read(*m.regs().find(name)); });
  }

  auto now() const -> vtime
  {
    return core::access(*on, 0, [](core::machine& m) { return m.virtual_clock().time(); });
  }
};

auto settles(std::function<bool()> const& reached, std::chrono::milliseconds limit = 5s) -> bool
{
  auto const until = std::chrono::steady_clock::now() + limit;
  while(not reached())
  {
    if(std::chrono::steady_clock::now() > until)
    {
      return false;
    }
    std::this_thread::sleep_for(100us);
  }
  return true;
}

auto drive(core::machine& m, core::run_goal const& goal, core::slice_bounds bounds)
    -> core::goal_result
{
  auto   remaining = goal.budget;
  auto   left      = goal.exact;
  icount executed{};
  for(;;)
  {
    auto const slice = m.run_slice({
        .max_instructions   = bounds.max_instructions,
        .wall_budget        = long_wall,
        .until              = goal.until,
        .budget_left        = remaining,
        .exact_instructions = left,
        .until_time         = goal.until_time,
        .leave_until        = executed.value == 0,
    });
    executed.value += slice.executed.value;
    if(slice.stopped)
    {
      return {.stopped = *slice.stopped, .executed = executed};
    }
    if(remaining)
    {
      remaining = icount{remaining->value - std::min(remaining->value, slice.executed.value)};
    }
    if(left)
    {
      left = icount{
          std::max<std::uint64_t>(left->value - std::min(left->value, slice.executed.value), 1)};
    }
  }
}

auto alu_regs(core::machine& m) -> std::vector<std::uint64_t>
{
  std::vector<std::uint64_t> values;
  for(auto const* name : {"r0", "r1", "r2", "r3", "pc"})
  {
    values.push_back(m.regs().read(*m.regs().find(name)));
  }
  return values;
}

auto thread_cpu(pthread_t thread) -> std::chrono::nanoseconds
{
  clockid_t clock{};
  REQUIRE(pthread_getcpuclockid(thread, &clock) == 0);
  timespec spent{};
  REQUIRE(clock_gettime(clock, &spent) == 0);
  return std::chrono::seconds{spent.tv_sec} + std::chrono::nanoseconds{spent.tv_nsec};
}

void require_error(std::exception_ptr const& error, auto check)
{
  REQUIRE(error);
  try
  {
    std::rethrow_exception(error);
  }
  catch(...)
  {
    check();
  }
}
}

TEST_CASE("parked access runs on the caller thread and starts no host thread")
{
  std::atomic<int> started{0};
  hosted           h{alu(10), {}, {.thread_started = [&started] { ++started; }}};
  auto const       pc = core::access(*h.on, 0, [](core::machine& m) {
    return std::pair{m.regs().pc(), std::this_thread::get_id()};
  });
  CHECK(pc.first == sym("alu", "alu_entry"));
  CHECK(pc.second == std::this_thread::get_id());
  auto const at = h.post([](core::machine& m) { m.regs().write(*m.regs().find("r0"), 5); });
  CHECK(at.done());
  CHECK(at.get() == vtime{0});
  CHECK(h.reg("r0") == 5);
  CHECK(h.on->state(0) == core::host_state::parked);
  CHECK(started == 0);
}

TEST_CASE("a goal to an address equals a run_slice loop on a twin machine")
{
  core::slice_bounds const bounds{.max_instructions = icount{10'000}};
  core::run_goal const     goal{.until = sym("alu", "alu_done")};
  auto                     twin     = alu(100'000);
  auto const               expected = drive(*twin, goal, bounds);
  hosted                   h{alu(100'000), bounds};
  auto const               got = h.run(goal).get();
  CHECK(got.stopped.reason == stop_reason::address);
  CHECK(got.stopped.reason == expected.stopped.reason);
  CHECK(got.stopped.instructions == expected.stopped.instructions);
  CHECK(got.stopped.time == expected.stopped.time);
  CHECK(got.stopped.where.pc == expected.stopped.where.pc);
  CHECK(got.executed == expected.executed);
  CHECK(h.on->state(0) == core::host_state::parked);
}

TEST_CASE("a budget carried across slices stops where one slice with the budget stops")
{
  auto       twin      = alu(1'000'000);
  auto const one_slice = twin->run_slice(
      {.max_instructions = icount{1'000'000},
       .wall_budget      = long_wall,
       .budget_left      = icount{10'000}});
  REQUIRE(one_slice.stopped.has_value());
  hosted     h{alu(1'000'000), {.max_instructions = icount{1000}}};
  auto const got = h.run({.budget = icount{10'000}}).get();
  CHECK(got.stopped.reason == stop_reason::budget);
  CHECK(got.stopped.instructions == one_slice.stopped->instructions);
  CHECK(got.executed == one_slice.executed);
}

TEST_CASE("an exact step of 7 executes 7 instructions")
{
  hosted     h{alu(1000)};
  auto const got = h.run({.exact = icount{7}}).get();
  CHECK(got.stopped.reason == stop_reason::budget);
  CHECK(got.executed == icount{7});
  CHECK(got.stopped.instructions == icount{7});
}

TEST_CASE("a goal of budget 0 stops at once and a goal at its until address leaves it")
{
  hosted     h{alu(1000)};
  auto const none = h.run({.budget = icount{0}}).get();
  CHECK(none.stopped.reason == stop_reason::budget);
  CHECK(none.executed == icount{0});
  auto const body  = sym("alu", "alu_body");
  auto const first = h.run({.until = body}).get();
  CHECK(first.stopped.where.pc == body);
  auto const again = h.run({.until = body}).get();
  CHECK(again.stopped.reason == stop_reason::address);
  CHECK(again.executed == icount{5});
}

TEST_CASE("read commands while running land at boundaries and leave the goal unchanged")
{
  constexpr std::uint32_t loops = 2'000'000;
  core::run_goal const    goal{.until = sym("alu", "alu_done")};
  auto                    twin       = alu(loops);
  auto const              expected   = drive(*twin, goal, {});
  auto const              twin_regs  = alu_regs(*twin);
  auto const              twin_trace = twin->trace().records();

  hosted                                h{alu(loops)};
  auto const                            start = h.now();
  auto const                            done  = h.run(goal);
  std::vector<core::host_future<vtime>> reads;
  std::atomic<std::uint64_t>            seen{0};
  for(int i = 0; i < 1000; ++i)
  {
    reads.push_back(
        h.post([&seen](core::machine& m) { seen = m.regs().read(*m.regs().find("r2")); }));
  }
  auto const got     = done.get();
  auto       running = 0;
  for(auto const& read : reads)
  {
    auto const at = read.get();
    CHECK(at >= start);
    CHECK(at <= got.stopped.time);
    running += at < got.stopped.time ? 1 : 0;
  }
  MESSAGE("reads served while running: " << running);
  CHECK(running > 0);
  CHECK(got.stopped.reason == expected.stopped.reason);
  CHECK(got.stopped.instructions == expected.stopped.instructions);
  CHECK(got.executed == expected.executed);
  core::access(*h.on, 0, [&](core::machine& m) {
    CHECK(alu_regs(m) == twin_regs);
    CHECK(m.trace().records() == twin_trace);
  });
}

TEST_CASE("a command posted while running completes after the lock is dropped")
{
  hosted h{alu(0)};
  h.start();
  REQUIRE(settles([&] { return h.on->state(0) == core::host_state::running; }));
  core::host_future<vtime> read;
  {
    auto held = h.on->lock();
    CHECK_FALSE(h.on->parked(held, 0));
    read = h.on->post(held, 0, [](core::machine&) {});
  }
  CHECK(read.wait_for(5s));
  std::ignore = read.get();
  CHECK_FALSE(h.on->halt(0).has_value());
}

TEST_CASE("halt from another thread parks a free run within 10 ms")
{
  hosted h{alu(0)};
  h.start();
  REQUIRE(settles([&] { return h.on->state(0) == core::host_state::running; }));
  std::this_thread::sleep_for(20ms);
  auto const before = std::chrono::steady_clock::now();
  auto const end    = h.on->halt(0);
  auto const took   = std::chrono::steady_clock::now() - before;
  CHECK_FALSE(end.has_value());
  CHECK(took < 10ms);
  CHECK(h.on->state(0) == core::host_state::parked);
  CHECK_FALSE(h.on->halt(0).has_value());
}

TEST_CASE("an idle free run sleeps and a posted command wakes it")
{
  std::atomic<pthread_t> thread{};
  hosted h{idle_forever(), {}, {.thread_started = [&thread] { thread = pthread_self(); }}};
  h.start();
  REQUIRE(settles([&] { return h.on->state(0) == core::host_state::idle_wait; }));
  auto const spent_before = thread_cpu(thread);
  std::this_thread::sleep_for(200ms);
  auto const spent = thread_cpu(thread) - spent_before;
  MESSAGE("host thread CPU over 200 ms idle: " << spent.count() << " ns");
  CHECK(spent < 5ms);
  auto const read = h.post([](core::machine&) {});
  CHECK(read.wait_for(5s));
  CHECK(settles([&] { return h.on->state(0) == core::host_state::idle_wait; }));
  CHECK_FALSE(h.on->halt(0).has_value());
}

TEST_CASE("a handler exception fails its goal with the same type and the next goal runs")
{
  auto   made       = alu(1000);
  auto   throw_once = std::make_shared<bool>(true);
  hosted h;
  h.hooks.push_back(
      made->add_intercept(sym("alu", "alu_body"), [throw_once](core::intercept_call const&) {
        if(std::exchange(*throw_once, false))
        {
          throw test_failure{"boom"};
        }
        return core::intercept_result::proceed();
      }));
  h.adopt(std::move(made));
  CHECK_THROWS_AS(h.run({.until = sym("alu", "alu_done")}).get(), test_failure);
  CHECK(h.on->state(0) == core::host_state::parked);
  auto const next = h.run({.until = sym("alu", "alu_done")}).get();
  CHECK(next.stopped.reason == stop_reason::address);
}

TEST_CASE("a free run ended by a handler holds the end until halt")
{
  auto   made       = alu(1000);
  auto   throw_once = std::make_shared<std::atomic<bool>>(true);
  hosted h;
  h.hooks.push_back(
      made->add_intercept(sym("alu", "alu_done"), [throw_once](core::intercept_call const&) {
        if(throw_once->exchange(false))
        {
          throw test_failure{"held"};
        }
        return core::intercept_result::proceed();
      }));
  h.adopt(std::move(made));
  core::host_future<popcorn::stop> pending;
  {
    auto held = h.on->lock();
    h.on->start(held, 0);
    pending = h.on->wait(held, 0, {.ready = [] { return false; }}).future();
  }
  CHECK_THROWS_AS(pending.get(), test_failure);
  REQUIRE(settles([&] { return h.on->state(0) == core::host_state::parked; }));
  auto const later = h.wait({}).future();
  CHECK(later.done());
  CHECK_THROWS_AS(later.get(), test_failure);

  CHECK_THROWS_AS(
      core::access(*h.on, 0, [](core::machine&) { return 0; }), popcorn::machine_running);
  vtime      inside{};
  auto const direct = h.post([&inside](core::machine& m) { inside = m.virtual_clock().time(); });
  CHECK(direct.done());
  CHECK(direct.get() == inside);
  {
    auto held = h.on->lock();
    CHECK(h.on->busy(held, 0));
    CHECK(h.on->session_open(held, 0));
    CHECK(h.on->parked(held, 0));
    CHECK_THROWS_AS(h.on->start(held, 0), popcorn::machine_busy);
  }

  auto const end = h.on->halt(0);
  REQUIRE(end.has_value());
  require_error(std::get<std::exception_ptr>(*end), [] { CHECK_THROWS_AS(throw, test_failure); });
  CHECK(h.now() == inside);
  h.start();
  REQUIRE(settles([&] { return h.on->state(0) == core::host_state::running; }));
  CHECK_FALSE(h.on->halt(0).has_value());
}

TEST_CASE("a second goal while one runs throws machine_busy")
{
  hosted     h{alu(0)};
  auto const first = h.run({.until = sym("alu", "alu_done")});
  {
    auto held = h.on->lock();
    CHECK_THROWS_AS(std::ignore = h.on->run(held, 0, {}), popcorn::machine_busy);
  }
  h.on->cancel(0);
  CHECK_THROWS_AS(first.get(), core::operation_cancelled);
  CHECK(h.on->state(0) == core::host_state::parked);
}

TEST_CASE("sync access while running throws machine_running at once")
{
  hosted     h{alu(0)};
  auto const goal = h.run({.until = sym("alu", "alu_done")});
  REQUIRE(settles([&] { return h.on->state(0) == core::host_state::running; }));
  auto const before = std::chrono::steady_clock::now();
  CHECK_THROWS_AS(h.reg("r0"), popcorn::machine_running);
  CHECK(std::chrono::steady_clock::now() - before < 10ms);
  h.on->cancel(0);
  CHECK_THROWS_AS(goal.get(), core::operation_cancelled);
}

TEST_CASE("a hook that accesses its own host runs inline")
{
  auto                       made = alu(1000);
  hosted                     h;
  std::atomic<core::host*>   self{};
  std::atomic<std::uint64_t> pc{};
  h.hooks.push_back(made->add_intercept(
      sym("alu", "alu_body"),
      [&self, &pc](core::intercept_call const&) {
        pc = core::access(*self.load(), 0, [](core::machine& m) { return m.regs().pc(); });
        return core::intercept_result::proceed();
      },
      core::intercept_mode::once));
  h.adopt(std::move(made));
  self           = h.on.get();
  auto const got = h.run({.until = sym("alu", "alu_done")}).get();
  CHECK(got.stopped.reason == stop_reason::address);
  CHECK(pc == sym("alu", "alu_body"));
}

TEST_CASE("destruction while running, while idle and with a command in flight")
{
  SUBCASE("running")
  {
    hosted h{alu(0)};
    h.start();
    REQUIRE(settles([&] { return h.on->state(0) == core::host_state::running; }));
    h.on.reset();
  }
  SUBCASE("idle")
  {
    hosted h{idle_forever()};
    h.start();
    REQUIRE(settles([&] { return h.on->state(0) == core::host_state::idle_wait; }));
    h.on.reset();
  }
  SUBCASE("command in flight")
  {
    auto                     made = alu(0);
    hosted                   h;
    std::atomic<core::host*> self{};
    std::latch               in_hook{1};
    std::latch               posted{1};
    h.hooks.push_back(made->add_intercept(
        sym("alu", "alu_body"),
        [&self, &in_hook, &posted](core::intercept_call const&) {
          in_hook.count_down();
          posted.wait();
          self.load()->close();
          return core::intercept_result::proceed();
        },
        core::intercept_mode::once));
    h.adopt(std::move(made));
    self            = h.on.get();
    auto const goal = h.run({});
    in_hook.wait();
    auto const read = h.post([](core::machine&) {});
    posted.count_down();
    CHECK_THROWS_AS(read.get(), core::machine_closed);
    CHECK_THROWS_AS(goal.get(), core::machine_closed);
    REQUIRE(settles([&] { return h.on->state(0) == core::host_state::stopping; }));
    h.on->close();
    h.on->close();
    CHECK_NOTHROW(std::ignore = h.reg("r0"));
    auto held = h.on->lock();
    CHECK_THROWS_AS(h.on->start(held, 0), core::machine_closed);
  }
}

TEST_CASE("interrupts from another thread do not move a requested stop")
{
  auto const schedule = [](popcorn::testing::machine_fixture<stop_at_arch>& f) {
    std::ignore =
        f.module->attached_events().schedule(icount{200'000}, stop_at_arch::stop_event, 0);
  };
  popcorn::testing::machine_fixture<stop_at_arch> reference{m33(), "alu", core::machine_options{}};
  reference.set("r0", 0);
  schedule(reference);
  hosted     quiet{std::move(reference.target)};
  auto const expected = quiet.run({}).get();
  REQUIRE(expected.stopped.reason == stop_reason::user);

  popcorn::testing::machine_fixture<stop_at_arch> stressed{m33(), "alu", core::machine_options{}};
  stressed.set("r0", 0);
  schedule(stressed);
  auto* const target = stressed.target.get();
  hosted      h{
      std::move(stressed.target),
      {.max_instructions = icount{1'000'000'000}, .wall_budget = long_wall}
  };
  auto const goal = h.run({});
  int        sent = 0;
  while(sent < 10'000 and not goal.done())
  {
    target->interrupt();
    ++sent;
    std::this_thread::yield();
  }
  auto const got = goal.get();
  MESSAGE("interrupts sent: " << sent);
  CHECK(got.stopped.reason == stop_reason::user);
  CHECK(got.stopped.instructions == expected.stopped.instructions);
  CHECK(got.stopped.where.pc == expected.stopped.where.pc);
  CHECK(got.executed == expected.executed);
}

TEST_CASE("two members on one host each equal the same goal on its own host")
{
  core::run_goal const goal{.until = sym("alu", "alu_done")};
  hosted               first{alu(30'000)};
  hosted               second{alu(70'000)};
  auto const           a = first.run(goal).get();
  auto const           b = second.run(goal).get();

  std::vector<std::unique_ptr<core::machine>> members;
  members.push_back(alu(30'000));
  members.push_back(alu(70'000));
  core::host shared{std::move(members)};
  CHECK(shared.members() == 2);
  core::host_future<core::goal_result> fa;
  core::host_future<core::goal_result> fb;
  {
    auto held = shared.lock();
    fa        = shared.run(held, 0, goal);
    fb        = shared.run(held, 1, goal);
  }
  auto const ga = fa.get();
  auto const gb = fb.get();
  CHECK(ga.stopped.instructions == a.stopped.instructions);
  CHECK(ga.executed == a.executed);
  CHECK(gb.stopped.instructions == b.stopped.instructions);
  CHECK(gb.executed == b.executed);
}

TEST_CASE("the host thread holds no mutex in hooks and continuations")
{
  auto                         made = alu(1000);
  hosted                       h;
  std::atomic<core::host*>     self{};
  std::atomic<bool>            hook_locked{false};
  std::atomic<bool>            continuation_locked{false};
  std::atomic<std::thread::id> host_thread{};
  std::atomic<std::thread::id> continuation_thread{};
  h.hooks.push_back(made->add_intercept(
      sym("alu", "alu_body"),
      [&](core::intercept_call const&) {
        hook_locked = self.load()->try_lock().owns_lock();
        return core::intercept_result::proceed();
      },
      core::intercept_mode::once));
  h.adopt(
      std::move(made), {}, {.thread_started = [&] { host_thread = std::this_thread::get_id(); }});
  self = h.on.get();
  core::host_future<core::goal_result> goal;
  {
    auto held = h.on->lock();
    goal      = h.on->run(held, 0, {.until = sym("alu", "alu_done")});
    goal.on_done([&] noexcept {
      continuation_locked = self.load()->try_lock().owns_lock();
      continuation_thread = std::this_thread::get_id();
    });
  }
  std::ignore = goal.get();
  REQUIRE(settles([&] { return continuation_thread.load() != std::thread::id{}; }));
  CHECK(hook_locked);
  CHECK(continuation_locked);
  CHECK(continuation_thread.load() == host_thread.load());
}

TEST_CASE("a hook inside a parked access owns the engine and keeps the mutex")
{
  auto                     made = alu(1000);
  hosted                   h;
  std::atomic<core::host*> self{};
  std::atomic<bool>        owned{false};
  std::atomic<bool>        other_locked{true};
  h.hooks.push_back(made->add_intercept(
      sym("alu", "alu_body"),
      [&](core::intercept_call const&) {
        owned = self.load()->owns_engine();
        std::thread{[&] { other_locked = self.load()->try_lock().owns_lock(); }}.join();
        return core::intercept_result::proceed();
      },
      core::intercept_mode::once));
  h.adopt(std::move(made));
  self = h.on.get();
  core::access(*h.on, 0, [](core::machine& m) {
    std::ignore = m.run_slice({.wall_budget = long_wall, .budget_left = icount{100}});
  });
  CHECK(owned);
  CHECK_FALSE(other_locked);
  CHECK_FALSE(h.on->owns_engine());
}

TEST_CASE("before_block runs for cancel, halt and close only, on the calling thread")
{
  std::atomic<int>             blocks{0};
  std::atomic<std::thread::id> blocked_on{};
  hosted                       h{alu(0), {}, {.before_block = [&] {
             ++blocks;
             blocked_on = std::this_thread::get_id();
                                 }}};
  auto                         goal = h.run({});
  {
    auto held   = h.on->lock();
    std::ignore = h.on->busy(held, 0);
    std::ignore = h.on->session_open(held, 0);
    std::ignore = h.on->post(held, 0, [](core::machine&) {});
  }
  CHECK(blocks == 0);
  h.on->cancel(0);
  CHECK(blocks == 1);
  h.start();
  std::ignore = h.wait({});
  CHECK(blocks == 1);
  std::ignore = h.on->halt(0);
  CHECK(blocks == 2);
  h.on->close();
  CHECK(blocks == 3);
  CHECK(blocked_on.load() == std::this_thread::get_id());
}

TEST_CASE("access to a second host from a parked section refuses a taken mutex at once")
{
  hosted      a{alu(10)};
  hosted      b{alu(10)};
  std::latch  b_held{1};
  std::latch  done{1};
  std::thread holder{[&] {
    auto held = b.on->lock();
    b_held.count_down();
    done.wait();
  }};
  b_held.wait();
  core::access(*a.on, 0, [&](core::machine&) {
    CHECK_THROWS_AS(
        core::access(*b.on, 0, [](core::machine&) { return 0; }), popcorn::machine_running);
    CHECK_THROWS_AS(b.on->halt(0), popcorn::machine_running);
    CHECK_THROWS_AS(b.on->cancel(0), popcorn::machine_running);
    CHECK_THROWS_AS(b.on->close(), popcorn::machine_running);
  });
  done.count_down();
  holder.join();
}

TEST_CASE("cancel, halt and close on a host thread only request")
{
  auto                     made = alu(0);
  hosted                   h;
  hosted                   other{alu(0)};
  std::atomic<core::host*> self{};
  std::atomic<bool>        returned{false};
  other.start();
  h.hooks.push_back(made->add_intercept(
      sym("alu", "alu_body"),
      [&](core::intercept_call const&) {
        self.load()->cancel(0);
        std::ignore = other.on->halt(0);
        returned    = true;
        return core::intercept_result::proceed();
      },
      core::intercept_mode::once));
  h.adopt(std::move(made));
  self = h.on.get();
  CHECK_THROWS_AS(h.run({}).get(), core::operation_cancelled);
  CHECK(returned);
  REQUIRE(settles([&] { return other.on->state(0) == core::host_state::parked; }));
  auto held = other.on->lock();
  CHECK_FALSE(other.on->session_open(held, 0));
}

TEST_CASE("a host destroyed on another host's thread takes the orphan path")
{
  std::atomic<std::thread::id> started_destroyed{};
  std::atomic<std::thread::id> fresh_destroyed{};
  std::atomic<std::thread::id> victim_thread{};
  std::atomic<std::thread::id> killer_thread{};

  popcorn::testing::machine_fixture<recording_arch> started{
      m33(), "alu", core::machine_options{}, &started_destroyed};
  started.set("r0", 0);
  auto victim = std::make_shared<hosted>(
      std::move(started.target), core::slice_bounds{},
      core::host_hooks{.thread_started = [&] { victim_thread = std::this_thread::get_id(); }});
  victim->start();
  REQUIRE(settles([&] { return victim->on->state(0) == core::host_state::running; }));

  popcorn::testing::machine_fixture<recording_arch> fresh_fixture{
      m33(), "alu", core::machine_options{}, &fresh_destroyed};
  auto fresh = std::make_shared<hosted>(std::move(fresh_fixture.target));

  auto   made = alu(1000);
  hosted killer;
  killer.hooks.push_back(made->add_intercept(
      sym("alu", "alu_body"),
      [&](core::intercept_call const&) {
        killer_thread = std::this_thread::get_id();
        victim.reset();
        fresh.reset();
        return core::intercept_result::proceed();
      },
      core::intercept_mode::once));
  killer.adopt(std::move(made));
  CHECK(killer.run({.until = sym("alu", "alu_done")}).get().stopped.reason == stop_reason::address);
  CHECK(fresh_destroyed.load() == killer_thread.load());
  REQUIRE(settles([&] { return started_destroyed.load() != std::thread::id{}; }));
  CHECK(started_destroyed.load() == victim_thread.load());
}

TEST_CASE("close from two threads at once joins once")
{
  for(int round = 0; round < 50; ++round)
  {
    hosted h{alu(0)};
    h.start();
    std::latch  go{2};
    std::thread first{[&] {
      go.arrive_and_wait();
      h.on->close();
    }};
    std::thread second{[&] {
      go.arrive_and_wait();
      h.on->close();
    }};
    first.join();
    second.join();
    CHECK(h.on->state(0) == core::host_state::stopping);
  }
}

TEST_CASE("waiters resolve on readiness and on deadlines, a throwing predicate fails alone")
{
  hosted h{idle_forever()};
  h.start();
  REQUIRE(settles([&] { return h.on->state(0) == core::host_state::idle_wait; }));
  auto const now      = h.wait({.deadline = vtime{0}}).future();
  auto const later_ps = vtime{5'000'000'000};
  auto       deadline = h.wait({.deadline = later_ps}).future();
  auto throwing = h.wait({.ready = []() -> bool { throw test_failure{"predicate"}; }}).future();
  auto never    = h.wait({.ready = [] { return false; }});
  auto ready    = h.wait({.ready = [] { return true; }}).future();
  CHECK(now.get().reason == stop_reason::time);
  CHECK(deadline.get().reason == stop_reason::time);
  CHECK(deadline.get().time == later_ps);
  CHECK_THROWS_AS(throwing.get(), test_failure);
  CHECK(ready.get().reason == stop_reason::user);
  CHECK_FALSE(never.future().done());
  never.drop();
  CHECK_THROWS_AS(never.future().get(), core::operation_cancelled);
  CHECK(h.on->state(0) != core::host_state::parked);
  CHECK_FALSE(h.on->halt(0).has_value());
}

TEST_CASE("a free run calls a waiter's predicate once per slice, not per event")
{
  popcorn::testing::machine_fixture<ticking_arch> f{m33(), "alu", core::machine_options{}};
  f.set("r0", 0);
  f.module->arm();
  auto* const arch = f.module;
  hosted      h{
      std::move(f.target), {.max_instructions = icount{100'000}, .wall_budget = long_wall}
  };
  int calls = 0;
  h.start();
  auto const waiting = h.wait({
      .ready =
          [&calls] {
            ++calls;
            return false;
          },
      .deadline = at_instruction(1'000'000),
  });
  CHECK(waiting.future().get().reason == stop_reason::time);
  CHECK_FALSE(h.on->halt(0).has_value());
  CHECK(arch->ticks >= 900);
  CHECK(calls <= 12);
}

auto ticking() -> std::pair<std::unique_ptr<core::machine>, ticking_arch*>
{
  popcorn::testing::machine_fixture<ticking_arch> f{m33(), "alu", core::machine_options{}};
  f.set("r0", 0);
  f.module->arm();
  auto* const arch = f.module;
  return {std::move(f.target), arch};
}

auto counter_at_least(std::uint32_t count) -> core::condition
{
  return {
      .where    = ticking_arch::counter_at,
      .size     = 4,
      .bits     = 32,
      .encoding = core::condition::kind::unsigned_int,
      .compare  = core::condition::op::ge,
      .value    = count,
  };
}

constexpr core::slice_bounds huge_slices{
    .max_instructions = icount{1'000'000'000},
    .wall_budget      = long_wall,
};

auto start_waiting(hosted const& h, std::vector<core::waiter_spec> specs)
    -> std::vector<core::waiter_handle>
{
  auto held = h.on->lock();
  h.on->start(held, 0);
  std::vector<core::waiter_handle> handles;
  for(auto& spec : specs)
  {
    handles.push_back(h.on->wait(held, 0, std::move(spec)));
  }
  return handles;
}

TEST_CASE("a condition waiter fires at the event a parked goal stops at, with mask bit 0")
{
  auto [reference, unused] = ticking();
  auto* const parked       = reference.get();
  hosted      quiet{std::move(reference), huge_slices};
  auto const  expected =
      quiet
          .run(
              {.stop_when =
                   [parked] { return core::condition_holds(counter_at_least(50), *parked); }})
          .get();
  REQUIRE(expected.stopped.reason == stop_reason::user);

  auto [member, arch] = ticking();
  hosted     h{std::move(member), huge_slices};
  auto const handles = start_waiting(
      h,
      {
          {
           .deadline = at_instruction(10'000'000),
           .when = core::condition_set{.members = {counter_at_least(50), counter_at_least(60)}},
           }
  });
  auto const& waiting = handles.front();
  auto const  got     = waiting.future().get();
  CHECK(got.reason == stop_reason::user);
  CHECK(got.instructions == expected.stopped.instructions);
  CHECK(got.where.pc == expected.stopped.where.pc);
  CHECK(waiting.fired() == 1);
  CHECK_FALSE(h.on->halt(0).has_value());
  CHECK(arch->ticks >= 50);
}

TEST_CASE("a condition already true fires at the boundary its posting ends the slice at")
{
  auto [member, arch] = ticking();
  hosted h{std::move(member), huge_slices};
  h.start();
  auto const waiting = h.wait({
      .deadline = at_instruction(500'000'000),
      .when     = core::condition_set{.members = {counter_at_least(0)}},
  });
  auto const got     = waiting.future().get();
  CHECK(got.reason == stop_reason::user);
  CHECK(got.instructions.value < 100'000'000);
  CHECK(waiting.fired() == 1);
  CHECK_FALSE(h.on->halt(0).has_value());
  CHECK(arch->ticks < 100'000);
}

auto flag_from(core::machine* target, vtime from) -> core::host_hooks
{
  return {.boundary = [target, from](core::member_id) {
    if(target->virtual_clock().time() >= from)
    {
      std::ignore = target->mem().write(ticking_arch::counter_at, 4, 1);
    }
  }};
}

TEST_CASE("a condition made true at its deadline's boundary ends with time, as a parked goal")
{
  auto const deadline  = at_instruction(200'000);
  auto       reference = alu(1'000'000);
  auto*      parked    = reference.get();
  hosted     quiet{std::move(reference), huge_slices, flag_from(parked, deadline)};
  auto const expected =
      quiet
          .run({
              .until_time = deadline,
              .stop_when = [parked] { return core::condition_holds(counter_at_least(1), *parked); },
          })
          .get();
  REQUIRE(expected.stopped.reason == stop_reason::time);

  auto       member  = alu(1'000'000);
  auto*      running = member.get();
  hosted     h{std::move(member), huge_slices, flag_from(running, deadline)};
  auto const handles = start_waiting(
      h,
      {
          {.deadline = deadline, .when = core::condition_set{.members = {counter_at_least(1)}}}
  });
  auto const& waiting = handles.front();
  auto const  got     = waiting.future().get();
  CHECK(got.reason == stop_reason::time);
  CHECK(got.time == expected.stopped.time);
  CHECK(waiting.fired() == 0);
  CHECK_FALSE(h.on->halt(0).has_value());
}

TEST_CASE("a free run ended after the boundary where a condition held reports no member fired")
{
  auto [member, arch] = ticking();
  auto* const target  = member.get();
  hosted      h{std::move(member), huge_slices, {.boundary = [target](core::member_id) {
             if(core::condition_holds(counter_at_least(5), *target))
             {
               throw test_failure{"after the slice"};
             }
                }}};
  auto const  handles = start_waiting(
      h, {
             {
              .deadline = at_instruction(10'000'000),
              .when     = core::condition_set{.members = {counter_at_least(5)}},
              }
  });
  auto const& waiting = handles.front();
  CHECK_THROWS_AS(waiting.future().get(), test_failure);
  CHECK(waiting.fired() == 0);
  auto const end = h.on->halt(0);
  REQUIRE(end.has_value());
  require_error(std::get<std::exception_ptr>(*end), [] { CHECK_THROWS_AS(throw, test_failure); });
}

struct storing
{
  std::unique_ptr<core::machine> target;
  core::watch_handle             watch;
};

auto storing_until_mail(std::uint32_t mail_at) -> storing
{
  popcorn::testing::machine_fixture<> f{m33(), "alu", core::machine_options{}};
  f.set("pc", sym("alu", "store_entry"));
  f.set("r0", 1'000'000);
  f.set("r2", ticking_arch::counter_at);
  auto* const target = f.target.get();
  auto        watch  = target->add_watch(
      ticking_arch::counter_at, 4, core::watch_kind::write,
      [target, mail_at](core::watch_hit const& hit) {
        if(hit.value == mail_at)
        {
          target->interrupt();
        }
        return core::watch_action::resume;
      });
  return {std::move(f.target), std::move(watch)};
}

TEST_CASE("mail landing in a slice's last block before its deadline ends it with time")
{
  constexpr std::uint32_t mailed    = 1'000;
  auto const              deadline  = at_instruction(2 + 5 * mailed);
  auto                    parked    = storing_until_mail(mailed);
  auto* const             reference = parked.target.get();
  hosted                  quiet{std::move(parked.target), huge_slices};
  auto const              expected =
      quiet
          .run({
              .until_time = deadline,
              .stop_when =
                  [reference] {
                    return core::condition_holds(counter_at_least(mailed), *reference);
                  },
          })
          .get();
  CHECK(expected.stopped.reason == stop_reason::time);

  auto       running = storing_until_mail(mailed);
  hosted     h{std::move(running.target), huge_slices};
  auto const handles = start_waiting(
      h, {
             {.deadline = deadline,
              .when     = core::condition_set{.members = {counter_at_least(mailed)}}}
  });
  auto const& waiting = handles.front();
  auto const  got     = waiting.future().get();
  CHECK(got.reason == stop_reason::time);
  CHECK(got.time == expected.stopped.time);
  CHECK(waiting.fired() == 0);
  CHECK_FALSE(h.on->halt(0).has_value());
}

TEST_CASE("plain memory is one mapped region, a condition elsewhere never holds")
{
  popcorn::testing::machine_fixture<> f{m33(), "alu", core::machine_options{}};
  auto                                tested = counter_at_least(0);
  tested.where                               = 0x6000'0000;
  CHECK_FALSE(core::condition_holds(tested, *f.target));
  CHECK_FALSE(f.target->mem().is_plain_memory(0x6000'0000, 4));
  CHECK_FALSE(f.target->mem().is_plain_memory(0x4001'0000, 4));
  CHECK(f.target->mem().is_plain_memory(0x2000'0000, 8));
  CHECK_FALSE(f.target->mem().is_plain_memory(0x200b'fffc, 8));
}

TEST_CASE("halt on a parked member returns at once and fails pending waiters")
{
  hosted h{alu(10)};
  CHECK_FALSE(h.on->halt(0).has_value());
  h.start();
  auto const pending = h.wait({.ready = [] { return false; }}).future();
  CHECK_FALSE(h.on->halt(0).has_value());
  CHECK_THROWS_AS(pending.get(), core::operation_cancelled);
}

TEST_CASE("an interrupt landing with a trap keeps the trap's stop")
{
  popcorn::testing::machine_fixture<poking_arch> f{m33(), "idle", core::machine_options{}};
  f.module->poke  = f.target.get();
  auto const stop = f.run_to_stop();
  CHECK(stop.reason == stop_reason::user);
  CHECK(f.module->poke == nullptr);
  auto const after = f.run({.max_instructions = icount{10}, .wall_budget = long_wall});
  CHECK(after.executed == icount{0});
}

TEST_CASE("halt from a host thread on a held end closes the session in place")
{
  auto                         made       = alu(1000);
  auto                         throw_once = std::make_shared<std::atomic<bool>>(true);
  std::atomic<int>             blocks{0};
  std::atomic<std::thread::id> blocked_on{};
  hosted                       victim;
  victim.hooks.push_back(
      made->add_intercept(sym("alu", "alu_done"), [throw_once](core::intercept_call const&) {
        if(throw_once->exchange(false))
        {
          throw test_failure{"held"};
        }
        return core::intercept_result::proceed();
      }));
  victim.adopt(std::move(made), {}, {.before_block = [&] {
                 ++blocks;
                 blocked_on = std::this_thread::get_id();
               }});
  victim.start();
  REQUIRE(settles([&] { return victim.on->state(0) == core::host_state::parked; }));

  auto                          driver = alu(1000);
  hosted                        killer;
  std::optional<core::held_end> end;
  std::atomic<std::thread::id>  killer_thread{};
  killer.hooks.push_back(driver->add_intercept(
      sym("alu", "alu_body"),
      [&](core::intercept_call const&) {
        killer_thread = std::this_thread::get_id();
        end           = victim.on->halt(0);
        return core::intercept_result::proceed();
      },
      core::intercept_mode::once));
  killer.adopt(std::move(driver));
  std::ignore = killer.run({.until = sym("alu", "alu_done")}).get();
  REQUIRE(end.has_value());
  require_error(std::get<std::exception_ptr>(*end), [] { CHECK_THROWS_AS(throw, test_failure); });
  CHECK(blocks == 1);
  CHECK(blocked_on.load() == killer_thread.load());
  {
    auto held = victim.on->lock();
    CHECK_FALSE(victim.on->session_open(held, 0));
  }

  victim.start();
  auto const pending = victim.wait({.ready = [] { return false; }}).future();
  REQUIRE(settles([&] { return victim.on->state(0) == core::host_state::running; }));
  std::this_thread::sleep_for(20ms);
  CHECK_FALSE(pending.done());
  CHECK(victim.on->state(0) == core::host_state::running);
  CHECK_FALSE(victim.on->halt(0).has_value());
  CHECK_THROWS_AS(pending.get(), core::operation_cancelled);
}

TEST_CASE("an idle host destroyed on another host's thread destroys its machine on its own thread")
{
  for(int round = 0; round < 20; ++round)
  {
    std::atomic<std::thread::id>                      destroyed{};
    std::atomic<std::thread::id>                      victim_thread{};
    popcorn::testing::machine_fixture<recording_arch> f{
        m33(), "idle", core::machine_options{}, &destroyed};
    REQUIRE(f.run_to_stop().reason == stop_reason::user);
    f.set("pc", sym("idle", "idle_forever"));
    auto victim = std::make_shared<hosted>(
        std::move(f.target), core::slice_bounds{},
        core::host_hooks{.thread_started = [&] { victim_thread = std::this_thread::get_id(); }});
    victim->start();
    REQUIRE(settles([&] { return victim->on->state(0) == core::host_state::idle_wait; }));

    auto   made = alu(1000);
    hosted killer;
    killer.hooks.push_back(made->add_intercept(
        sym("alu", "alu_body"),
        [&](core::intercept_call const&) {
          victim.reset();
          return core::intercept_result::proceed();
        },
        core::intercept_mode::once));
    killer.adopt(std::move(made));
    std::ignore = killer.run({.until = sym("alu", "alu_done")}).get();
    REQUIRE(settles([&] { return destroyed.load() != std::thread::id{}; }));
    CHECK(destroyed.load() == victim_thread.load());
  }
}

TEST_CASE("no-op stamps leave a goal's results unchanged")
{
  core::run_goal const goal{.budget = icount{2'000'000}};
  auto const           observe = [](hosted const& h) {
    return core::access(*h.on, 0, [](core::machine& m) {
      return std::tuple{alu_regs(m), sram(m), m.trace().records()};
    });
  };
  hosted     plain{alu(0)};
  auto const expected       = plain.run(goal).get();
  auto const expected_state = observe(plain);

  hosted                                h{alu(0)};
  std::vector<core::host_future<vtime>> stamps;
  for(std::uint64_t k = 1; k <= 10; ++k)
  {
    stamps.push_back(h.post([](core::machine&) {}, at_instruction(150'000 * k + 7)));
    CHECK_FALSE(stamps.back().done());
  }
  auto const got = h.run(goal).get();
  CHECK(got.stopped.reason == expected.stopped.reason);
  CHECK(got.stopped.instructions == expected.stopped.instructions);
  CHECK(got.stopped.time == expected.stopped.time);
  CHECK(got.stopped.where.pc == expected.stopped.where.pc);
  CHECK(got.executed == expected.executed);
  auto const state = observe(h);
  CHECK(std::get<0>(state) == std::get<0>(expected_state));
  CHECK(std::get<1>(state) == std::get<1>(expected_state));
  CHECK(std::get<2>(state) == std::get<2>(expected_state));
  for(std::uint64_t k = 1; k <= 10; ++k)
  {
    auto const t = at_instruction(150'000 * k + 7);
    REQUIRE(stamps[k - 1].wait_for(5s));
    auto const at = stamps[k - 1].get();
    CHECK(at >= t);
    CHECK(at.ps < t.ps + 512 * ps_per_instruction);
  }
}

TEST_CASE("a stamp on an idle core applies exactly at its time")
{
  hosted     h{idle_forever()};
  auto const start = h.now();
  auto const t     = vtime{start.ps + 1'000'000'000};
  auto       seen  = std::make_shared<vtime>();
  auto const stamp = h.post([seen](core::machine& m) { *seen = m.virtual_clock().time(); }, t);
  CHECK_FALSE(stamp.done());
  auto const got = h.run({.until_time = vtime{start.ps + 2'000'000'000}}).get();
  CHECK(got.stopped.reason == stop_reason::time);
  CHECK(stamp.get() == t);
  CHECK(*seen == t);
}

TEST_CASE("a stamp posted to an idle free run applies exactly at its time")
{
  hosted     h{idle_forever()};
  auto const start = h.now();
  h.start();
  REQUIRE(settles([&] { return h.on->state(0) == core::host_state::idle_wait; }));
  auto const t     = vtime{start.ps + 1'000'000'000};
  auto const stamp = h.post([](core::machine&) {}, t);
  CHECK(stamp.get() == t);
  REQUIRE(settles([&] { return h.on->state(0) == core::host_state::idle_wait; }));
  CHECK_FALSE(h.on->halt(0).has_value());
  CHECK(h.now() == t);
}

TEST_CASE("a stamp on a running core lands within one block of its time")
{
  hosted     h{alu(0)};
  auto const t     = at_instruction(1'000'003);
  auto       seen  = std::make_shared<vtime>();
  auto const stamp = h.post([seen](core::machine& m) { *seen = m.virtual_clock().time(); }, t);
  auto const goal  = h.run({.budget = icount{20'000'000}});
  auto       best  = std::make_shared<std::atomic<std::uint64_t>>(0);
  auto const quick = h.post([best](core::machine& m) { *best = m.virtual_clock().time().ps; });
  std::ignore      = goal.get();
  CHECK(stamp.get() == *seen);
  CHECK(*seen >= t);
  CHECK(seen->ps < t.ps + 512 * ps_per_instruction);
  CHECK(quick.get().ps == best->load());
}

TEST_CASE("a stamped stimulus posted at a random wall time gives the same final state")
{
  auto const once = [](std::chrono::microseconds delay) {
    hosted                   h{alu(0)};
    auto const               goal = h.run({.budget = icount{40'000'000}});
    core::host_future<vtime> stamp;
    std::thread              poster{[&] {
      std::this_thread::sleep_for(delay);
      stamp = h.post(
          [](core::machine& m) { m.regs().write(*m.regs().find("r1"), 7); },
          at_instruction(30'000'001));
    }};
    poster.join();
    auto const got = goal.get();
    auto const at  = stamp.get();
    return std::tuple{got.stopped.instructions, at, core::access(*h.on, 0, [](core::machine& m) {
                        return alu_regs(m);
                      })};
  };
  std::random_device                          seed;
  std::uniform_int_distribution<std::int64_t> jitter{0, 999};
  auto const                                  first = once(std::chrono::microseconds{jitter(seed)});
  for(std::int64_t spread = 1; spread < 5; ++spread)
  {
    auto const delay = std::chrono::microseconds{spread * 1000 + jitter(seed)};
    CHECK(once(delay) == first);
  }
}

TEST_CASE("a past stamp throws parked and fails running, an equal stamp applies at once")
{
  hosted h{alu(0)};
  std::ignore = h.run({.budget = icount{1000}}).get();
  CHECK_THROWS_AS(std::ignore = h.post([](core::machine&) {}, vtime{0}), std::invalid_argument);
  try
  {
    std::ignore = h.post([](core::machine&) {}, vtime{0});
  }
  catch(std::invalid_argument const& error)
  {
    MESSAGE(error.what());
    CHECK(std::string_view{error.what()}.starts_with("stamp 0.000000 ms is before machine time"));
  }
  auto const now   = h.now();
  auto const equal = h.post([](core::machine&) {}, now);
  CHECK(equal.done());
  CHECK(equal.get() == now);

  auto const goal = h.run({});
  REQUIRE(settles([&] { return h.on->state(0) == core::host_state::running; }));
  auto const late = h.post([](core::machine&) {}, vtime{0});
  CHECK_THROWS_AS(late.get(), std::invalid_argument);
  h.on->cancel(0);
  CHECK_THROWS_AS(goal.get(), core::operation_cancelled);
}

TEST_CASE("stamps apply in time order, equal stamps in posting order")
{
  hosted                                h{alu(0)};
  auto                                  order = std::make_shared<std::vector<int>>();
  std::vector<core::host_future<vtime>> stamps;
  auto const                            log = [order](int id) {
    return [order, id](core::machine&) { order->push_back(id); };
  };
  stamps.push_back(h.post(log(2), at_instruction(20'000)));
  stamps.push_back(h.post(log(1), at_instruction(10'000)));
  stamps.push_back(h.post(log(3), at_instruction(30'000)));
  stamps.push_back(h.post(log(4), at_instruction(30'000)));
  for(int k = 0; k < 100; ++k)
  {
    stamps.push_back(h.post(log(10 + k), at_instruction(40'000)));
  }
  std::ignore = h.run({.budget = icount{100'000}}).get();
  for(auto const& stamp : stamps)
  {
    CHECK(stamp.done());
  }
  std::vector<int> expected{1, 2, 3, 4};
  for(int k = 0; k < 100; ++k)
  {
    expected.push_back(10 + k);
  }
  CHECK(*order == expected);
}

TEST_CASE("a reset or a restore fails pending stamps")
{
  SUBCASE("parked reset")
  {
    hosted h{alu(0)};
    std::ignore      = h.run({.budget = icount{1000}}).get();
    auto const stamp = h.post([](core::machine&) {}, at_instruction(5000));
    core::access(*h.on, 0, [](core::machine& m) { m.reset(); });
    std::ignore = h.run({.budget = icount{10'000}}).get();
    CHECK_THROWS_AS(stamp.get(), core::operation_cancelled);
  }
  SUBCASE("parked restore")
  {
    hosted     h{alu(0)};
    auto const saved = core::access(*h.on, 0, [](core::machine& m) { return m.snapshot(); });
    auto const stamp = h.post([](core::machine&) {}, at_instruction(5000));
    core::access(*h.on, 0, [&saved](core::machine& m) { m.restore(saved); });
    auto const after = h.post([](core::machine&) {}, at_instruction(6000));
    CHECK_THROWS_AS(stamp.get(), core::operation_cancelled);
    std::ignore = h.run({.budget = icount{10'000}}).get();
    CHECK(after.done());
  }
  SUBCASE("model-requested reset during a run")
  {
    popcorn::testing::machine_fixture<resetting_arch> f{
        m33(), "alu",
        core::machine_options{.on_reset_request = core::reset_request_policy::reset_and_continue}};
    f.set("r0", 0);
    std::ignore =
        f.module->attached_events().schedule(icount{200'000}, resetting_arch::reset_event, 0);
    hosted     h{std::move(f.target)};
    auto const stamp = h.post([](core::machine&) {}, at_instruction(600'000));
    std::ignore      = h.run({.budget = icount{1'000'000}}).get();
    CHECK_THROWS_AS(stamp.get(), core::operation_cancelled);
  }
}

TEST_CASE("a parked reset fails no stamp posted after it once a run or a free run began")
{
  SUBCASE("goal")
  {
    hosted h{alu(0)};
    core::access(*h.on, 0, [](core::machine& m) { m.reset(); });
    auto const [goal, stamp] = [&h] {
      auto held  = h.on->lock();
      auto ran   = h.on->run(held, 0, {.budget = icount{10'000}});
      auto taken = h.on->post(held, 0, [](core::machine&) {}, at_instruction(5000));
      return std::pair{std::move(ran), std::move(taken)};
    }();
    std::ignore = goal.get();
    CHECK(stamp.get() >= at_instruction(5000));
  }
  SUBCASE("free run")
  {
    hosted h{alu(0)};
    core::access(*h.on, 0, [](core::machine& m) { m.reset(); });
    auto const stamp = [&h] {
      auto held = h.on->lock();
      h.on->start(held, 0);
      return h.on->post(held, 0, [](core::machine&) {}, at_instruction(5000));
    }();
    CHECK(stamp.get() >= at_instruction(5000));
    std::ignore = h.on->halt(0);
  }
}

TEST_CASE("withdraw removes a pending stamp and refuses one that ran")
{
  hosted     h{alu(0)};
  auto       ran   = std::make_shared<std::atomic<int>>(0);
  auto const stamp = h.post([ran](core::machine&) { ++*ran; }, at_instruction(5000));
  {
    auto held = h.on->lock();
    CHECK(h.on->withdraw(held, 0, stamp));
    CHECK_FALSE(h.on->withdraw(held, 0, stamp));
  }
  CHECK_THROWS_AS(stamp.get(), core::operation_cancelled);
  auto const kept = h.post([ran](core::machine&) { *ran += 10; }, at_instruction(6000));
  std::ignore     = h.run({.budget = icount{10'000}}).get();
  CHECK(*ran == 10);
  auto held = h.on->lock();
  CHECK_FALSE(h.on->withdraw(held, 0, kept));
}

TEST_CASE("a stamped read on a parked machine resolves when a goal reaches it")
{
  auto const once = [] {
    hosted     h{alu(0)};
    auto       value = std::make_shared<std::uint64_t>(0);
    auto const read  = h.post(
        [value](core::machine& m) { *value = m.regs().read(*m.regs().find("r2")); },
        at_instruction(500'003));
    CHECK_FALSE(read.done());
    std::ignore = h.run({.budget = icount{1'000'000}}).get();
    return std::pair{read.get(), *value};
  };
  auto const first = once();
  CHECK(first.second != 0);
  CHECK(first == once());
}

TEST_CASE("stamp edge cases: at zero, never due, during exact stepping")
{
  hosted     fresh{alu(0)};
  auto const zero = fresh.post([](core::machine&) {}, vtime{0});
  CHECK(zero.done());
  CHECK(zero.get() == vtime{0});
  auto const never =
      fresh.post([](core::machine&) {}, vtime{std::numeric_limits<std::uint64_t>::max()});
  std::ignore = fresh.run({.budget = icount{1000}}).get();
  CHECK_FALSE(never.done());
  fresh.on->close();
  CHECK_THROWS_AS(never.get(), core::machine_closed);

  hosted     stepping{alu(0)};
  auto const t     = at_instruction(500);
  auto const stamp = stepping.post([](core::machine&) {}, t);
  auto const got   = stepping.run({.exact = icount{1000}}).get();
  CHECK(got.executed == icount{1000});
  CHECK(stamp.get() >= t);
  CHECK(stamp.get().ps < t.ps + 512 * ps_per_instruction);
}

TEST_CASE("stamps posted while running are checked when the goal stops in that slice")
{
  auto                       made = alu(1000);
  hosted                     h;
  std::latch                 in_hook{1};
  std::latch                 posted{1};
  std::atomic<std::uint64_t> hook_ps{};
  auto* const                target = made.get();
  h.hooks.push_back(made->add_intercept(
      sym("alu", "alu_body"),
      [&](core::intercept_call const&) {
        hook_ps = target->virtual_clock().time().ps;
        in_hook.count_down();
        posted.wait();
        return core::intercept_result::stop(stop_reason::user);
      },
      core::intercept_mode::once));
  h.adopt(std::move(made));
  auto const goal = h.run({});
  in_hook.wait();
  auto const parks_at = vtime{hook_ps.load()};
  auto const past     = h.post([](core::machine&) {}, vtime{0});
  auto const equal    = h.post([](core::machine&) {}, parks_at);
  posted.count_down();
  auto const got = goal.get();
  CHECK(got.stopped.reason == stop_reason::user);
  CHECK(got.stopped.time == parks_at);
  REQUIRE(past.wait_for(5s));
  CHECK_THROWS_AS(past.get(), std::invalid_argument);
  REQUIRE(equal.wait_for(5s));
  CHECK(equal.get() == parks_at);
}

TEST_CASE("stamps posted while running are checked when a halt parks in the same turn")
{
  auto                     made = alu(0);
  hosted                   h;
  std::atomic<core::host*> self{};
  std::latch               in_hook{1};
  std::latch               posted{1};
  h.hooks.push_back(made->add_intercept(
      sym("alu", "alu_body"),
      [&](core::intercept_call const&) {
        in_hook.count_down();
        posted.wait();
        std::ignore = self.load()->halt(0);
        return core::intercept_result::proceed();
      },
      core::intercept_mode::once));
  h.adopt(std::move(made));
  self = h.on.get();
  h.start();
  in_hook.wait();
  auto const past = h.post([](core::machine&) {}, vtime{0});
  posted.count_down();
  REQUIRE(past.wait_for(5s));
  CHECK_THROWS_AS(past.get(), std::invalid_argument);
  REQUIRE(settles([&] { return h.on->state(0) == core::host_state::parked; }));
  auto held = h.on->lock();
  CHECK_FALSE(h.on->session_open(held, 0));
}

TEST_CASE("a single-slice goal yields where one run_slice under its bounds yields")
{
  auto       twin     = alu(1'000'000);
  auto const expected = twin->run_slice(
      {.max_instructions = icount{1000}, .wall_budget = long_wall, .leave_until = true});
  REQUIRE_FALSE(expected.stopped.has_value());
  hosted     h{alu(1'000'000)};
  auto const got =
      h.run({
                .once =
                    core::single_slice{
                                       .bounds      = {.max_instructions = icount{1000}, .wall_budget = long_wall},
                                       .leave_until = true}
  })
          .get();
  CHECK(got.yielded);
  CHECK(got.executed == expected.executed);
  CHECK(h.reg("pc") == twin->regs().pc());
  CHECK(h.on->state(0) == core::host_state::parked);
  auto held = h.on->lock();
  CHECK_FALSE(h.on->busy(held, 0));
}

TEST_CASE("a single-slice goal reports a stop inside its slice and honours leave_until")
{
  hosted     h{alu(1'000'000)};
  auto const stopped =
      h.run({.budget = icount{10},
             .once   = core::single_slice{.bounds = {.max_instructions = icount{1000}}}})
          .get();
  CHECK_FALSE(stopped.yielded);
  CHECK(stopped.stopped.reason == stop_reason::budget);
  CHECK(stopped.executed == stopped.stopped.instructions);
  auto const at      = sym("alu", "alu_body");
  auto const reached = h.run({.until = at}).get();
  REQUIRE(reached.stopped.reason == stop_reason::address);
  auto const kept = h.run({.until = at, .once = core::single_slice{.leave_until = false}}).get();
  CHECK_FALSE(kept.yielded);
  CHECK(kept.stopped.reason == stop_reason::address);
  CHECK(kept.executed == icount{0});
  auto const left = h.run({.until = at, .once = core::single_slice{.leave_until = true}}).get();
  CHECK(left.executed.value > 0);
}

TEST_CASE("a parked section whose body accesses the same host again runs it inline")
{
  hosted     h{alu(10)};
  auto const nested = core::access(*h.on, 0, [&h](core::machine& outer) {
    return core::access(*h.on, 0, [&outer](core::machine& inner) {
      return std::pair{&inner == &outer, std::this_thread::get_id()};
    });
  });
  CHECK(nested.first);
  CHECK(nested.second == std::this_thread::get_id());
  CHECK_FALSE(h.on->owns_engine());
}

TEST_CASE("a goal's finish runs on the host thread before its future completes, a cancel too")
{
  hosted                       h{alu(100'000)};
  std::atomic<std::thread::id> ran_on{};
  std::atomic<int>             stopped{-1};
  auto                         recorder = [&](core::machine&, popcorn::stop const* stop) {
    ran_on  = std::this_thread::get_id();
    stopped = stop == nullptr ? 0 : static_cast<int>(stop->reason);
  };
  auto const done = h.run({.until = sym("alu", "alu_done"), .finish = recorder}).get();
  CHECK(done.stopped.reason == stop_reason::address);
  CHECK(stopped == static_cast<int>(stop_reason::address));
  CHECK(ran_on.load() != std::this_thread::get_id());
  auto endless = h.run({.finish = recorder});
  REQUIRE(settles([&h] { return h.on->state(0) == core::host_state::running; }));
  h.on->cancel(0);
  REQUIRE(endless.wait_for(5s));
  CHECK(stopped == 0);
  auto failed = core::host_future<core::goal_result>{};
  failed      = h.run({.budget = icount{10}, .finish = [](core::machine&, popcorn::stop const*) {
                    throw test_failure("finish failed");
                       }});
  require_error(
      [&failed] {
        try
        {
          std::ignore = failed.get();
        }
        catch(...)
        {
          return std::current_exception();
        }
        return std::exception_ptr{};
      }(),
      [] { CHECK_THROWS_AS(throw, test_failure); });
  auto held = h.on->lock();
  CHECK_FALSE(h.on->busy(held, 0));
}

TEST_CASE("a waiter's after counts from the boundary that registers it")
{
  hosted     h{idle_forever()};
  auto const before = h.now();
  h.start();
  auto const ended = h.wait({.after = vtime{1'000'000'000}}).future().get();
  CHECK(ended.reason == stop_reason::time);
  CHECK(ended.time.ps >= before.ps + 1'000'000'000);
  auto const fixed = h.wait({.deadline = ended.time, .after = vtime{5'000'000'000}}).future().get();
  CHECK(fixed.reason == stop_reason::time);
  CHECK(fixed.time == ended.time);
  CHECK_FALSE(h.on->halt(0).has_value());
}

TEST_CASE("a waiter posted inside a slice counts its after from a block after the post")
{
  popcorn::testing::machine_fixture<ticking_arch> f{m33(), "alu", core::machine_options{}};
  f.set("r0", 0);
  f.module->arm();
  auto* const arch = f.module;
  hosted      h{
      std::move(f.target), {.max_instructions = icount{1'000'000'000}, .wall_budget = long_wall}
  };
  h.start();
  std::this_thread::sleep_for(5ms);
  auto const ended =
      h.wait({.ready = [] { return false; }, .after = at_instruction(1'000)}).future().get();
  CHECK(ended.reason == stop_reason::time);
  CHECK(ended.instructions.value < 100'000'000);
  CHECK_FALSE(h.on->halt(0).has_value());
  CHECK(arch->ticks < 100'000);
}

TEST_CASE("the boundary hook runs on the host thread as the engine owner around every slice")
{
  std::atomic<int>  calls{0};
  std::atomic<bool> owned{true};
  std::atomic<bool> elsewhere{true};
  auto const        test_thread = std::this_thread::get_id();
  hosted            h;
  h.adopt(
      alu(1'000'000), {.max_instructions = icount{10'000}, .wall_budget = long_wall},
      {.boundary = [&](core::member_id who) {
        ++calls;
        owned     = owned and who == 0 and h.on->owns_engine();
        elsewhere = elsewhere and std::this_thread::get_id() != test_thread;
      }});
  auto const done = h.run({.budget = icount{100'000}}).get();
  CHECK(done.stopped.reason == stop_reason::budget);
  CHECK(calls >= 2 * 10);
  CHECK(owned);
  CHECK(elsewhere);
}

namespace
{
struct pipe_pair
{
  int read_end  = -1;
  int write_end = -1;

  pipe_pair()
  {
    int fds[2]{};
    REQUIRE(::pipe2(fds, O_NONBLOCK | O_CLOEXEC) == 0);
    read_end  = fds[0];
    write_end = fds[1];
  }

  pipe_pair(pipe_pair const&)                    = delete;
  auto operator=(pipe_pair const&) -> pipe_pair& = delete;

  ~pipe_pair()
  {
    ::close(read_end);
    ::close(write_end);
  }

  void put() const
  {
    char const byte = 'x';
    REQUIRE(::write(write_end, &byte, 1) == 1);
  }

  void take() const
  {
    char chunk[256];
    while(::read(read_end, chunk, sizeof chunk) > 0)
    {
    }
  }

  void fill() const
  {
    char const chunk[4096]{};
    while(::write(write_end, chunk, sizeof chunk) > 0)
    {
    }
  }
};
}

TEST_CASE("a watched descriptor wakes an idle free run, and not once the member parks")
{
  pipe_pair              input;
  std::atomic<int>       boundaries{0};
  std::atomic<pthread_t> thread{};
  hosted                 h{
      idle_forever(),
      {},
      {.thread_started = [&thread] { thread = pthread_self(); },
          .boundary =
           [&](core::member_id) {
             ++boundaries;
             input.take();
           }}
  };
  auto const id = h.on->watch(0, input.read_end, [] { return false; });
  h.start();
  REQUIRE(settles([&] { return h.on->state(0) == core::host_state::idle_wait; }));
  auto const idle_spent = thread_cpu(thread);
  std::this_thread::sleep_for(100ms);
  auto const idle = thread_cpu(thread) - idle_spent;
  MESSAGE("host thread CPU over 100 ms idle with a watch: " << idle.count() << " ns");
  CHECK(idle < 5ms);
  auto const before = boundaries.load();
  auto const woken  = std::chrono::steady_clock::now();
  input.put();
  CHECK(settles([&] { return boundaries.load() > before; }, 10ms));
  MESSAGE(
      "wake to boundary: "
      << std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now() - woken)
             .count()
      << " us");
  CHECK(settles([&] { return h.on->state(0) == core::host_state::idle_wait; }));
  std::ignore = h.on->halt(0);
  input.put();
  auto const parked_spent = thread_cpu(thread);
  std::this_thread::sleep_for(100ms);
  auto const spent = thread_cpu(thread) - parked_spent;
  MESSAGE("host thread CPU over 100 ms parked with unread input: " << spent.count() << " ns");
  CHECK(spent < 5ms);
  h.on->unwatch(id);
}

TEST_CASE("a watch wakes for writing while it wants to")
{
  pipe_pair         output;
  std::atomic<int>  boundaries{0};
  std::atomic<bool> wants{true};
  hosted            h{idle_forever(), {}, {.boundary = [&](core::member_id) { ++boundaries; }}};
  output.fill();
  auto const id = h.on->watch(0, output.write_end, [&wants] { return wants.load(); });
  h.start();
  REQUIRE(settles([&] { return h.on->state(0) == core::host_state::idle_wait; }));
  auto const full = boundaries.load();
  std::this_thread::sleep_for(20ms);
  CHECK(boundaries.load() == full);
  wants = false;
  output.take();
  CHECK(settles([&] { return boundaries.load() > full; }, 10ms));
  REQUIRE(settles([&] { return h.on->state(0) == core::host_state::idle_wait; }));
  auto const writable = boundaries.load();
  std::this_thread::sleep_for(20ms);
  CHECK(boundaries.load() == writable);
  h.on->unwatch(id);
  CHECK_FALSE(h.on->halt(0).has_value());
}

TEST_CASE("an unwatched descriptor no longer wakes the host")
{
  pipe_pair        input;
  std::atomic<int> boundaries{0};
  hosted           h{idle_forever(), {}, {.boundary = [&](core::member_id) { ++boundaries; }}};
  auto const       id = h.on->watch(0, input.read_end, [] { return false; });
  h.start();
  REQUIRE(settles([&] { return h.on->state(0) == core::host_state::idle_wait; }));
  h.on->unwatch(id);
  h.on->unwatch(id);
  REQUIRE(settles([&] { return h.on->state(0) == core::host_state::idle_wait; }));
  auto const before = boundaries.load();
  input.put();
  std::this_thread::sleep_for(20ms);
  CHECK(boundaries.load() == before);
  CHECK_FALSE(h.on->halt(0).has_value());
}

TEST_CASE("a boundary hook throwing after a slice fails the goal or ends the free run with it")
{
  std::atomic<int> calls{0};
  auto const       after_first = [&calls](core::member_id) {
    if(++calls == 2)
    {
      throw test_failure{"after the slice"};
    }
  };
  hosted goal_host{
      alu(1'000'000),
      {.max_instructions = icount{10'000}, .wall_budget = long_wall},
      {.boundary = after_first}
  };
  CHECK_THROWS_AS(goal_host.run({.budget = icount{100'000}}).get(), test_failure);
  CHECK(goal_host.on->state(0) == core::host_state::parked);

  calls = 0;
  hosted free_host{
      alu(1'000'000),
      {.max_instructions = icount{10'000}, .wall_budget = long_wall},
      {.boundary = after_first}
  };
  free_host.start();
  REQUIRE(settles([&] {
    auto held = free_host.on->lock();
    return free_host.on->parked(held, 0);
  }));
  auto const end = free_host.on->halt(0);
  REQUIRE(end.has_value());
  require_error(std::get<std::exception_ptr>(*end), [] { CHECK_THROWS_AS(throw, test_failure); });
}

TEST_CASE("watched descriptors the host cannot poll still wake an idle free run, late")
{
  pipe_pair                   input;
  std::atomic<int>            boundaries{0};
  hosted                      h{idle_forever(), {}, {.boundary = [&](core::member_id) {
             ++boundaries;
             input.take();
                                }}};
  std::vector<core::watch_id> ids;
  for(int copy = 0; copy < 32; ++copy)
  {
    ids.push_back(h.on->watch(0, input.read_end, [] { return false; }));
  }
  h.start();
  REQUIRE(settles([&] { return h.on->state(0) == core::host_state::idle_wait; }));
  rlimit saved{};
  REQUIRE(::getrlimit(RLIMIT_NOFILE, &saved) == 0);
  rlimit lowered   = saved;
  lowered.rlim_cur = 16;
  REQUIRE(::setrlimit(RLIMIT_NOFILE, &lowered) == 0);
  ids.push_back(h.on->watch(0, input.read_end, [] { return false; }));
  std::this_thread::sleep_for(30ms);
  REQUIRE(settles([&] { return h.on->state(0) == core::host_state::idle_wait; }));
  auto const before = boundaries.load();
  input.put();
  auto const woken = settles([&] { return boundaries.load() > before; }, 200ms);
  REQUIRE(::setrlimit(RLIMIT_NOFILE, &saved) == 0);
  CHECK(woken);
  for(auto const id : ids)
  {
    h.on->unwatch(id);
  }
  CHECK_FALSE(h.on->halt(0).has_value());
}

void end_by_handler(hosted& h)
{
  auto made       = alu(1000);
  auto throw_once = std::make_shared<std::atomic<bool>>(true);
  h.hooks.push_back(
      made->add_intercept(sym("alu", "alu_done"), [throw_once](core::intercept_call const&) {
        if(throw_once->exchange(false))
        {
          throw test_failure{"held"};
        }
        return core::intercept_result::proceed();
      }));
  h.adopt(std::move(made));
}

auto parked_now(hosted const& h) -> bool
{
  auto held = h.on->lock();
  return h.on->parked(held, 0);
}

TEST_CASE("a waiter completed at creation by a held end keeps no predicate")
{
  hosted h;
  end_by_handler(h);
  h.start();
  REQUIRE(settles([&] { return parked_now(h); }));
  auto                     sentinel = std::make_shared<int>();
  std::weak_ptr<int> const watched  = sentinel;
  auto const               handle   = h.wait({.ready = [sentinel] { return false; }});
  sentinel.reset();
  CHECK(handle.future().done());
  CHECK(watched.expired());
  CHECK(h.on->halt(0).has_value());
}

TEST_CASE("a dropped waiter wakes an idle free run, which releases its predicate")
{
  hosted h{idle_forever()};
  h.start();
  REQUIRE(settles([&] { return h.on->state(0) == core::host_state::idle_wait; }));
  auto                     sentinel = std::make_shared<int>();
  std::weak_ptr<int> const watched  = sentinel;
  std::atomic<int>         checks{0};
  auto                     handle = h.wait({.ready = [sentinel, &checks] {
    ++checks;
    return false;
  }});
  sentinel.reset();
  REQUIRE(
      settles([&] { return checks.load() > 0 and h.on->state(0) == core::host_state::idle_wait; }));
  auto const before = checks.load();
  handle.drop();
  CHECK(settles([&] { return watched.expired(); }, 500ms));
  CHECK(h.on->state(0) == core::host_state::idle_wait);
  CHECK(checks.load() == before);
  CHECK_FALSE(h.on->halt(0).has_value());
}

TEST_CASE("a predicate is released when the free run ends otherwise at a slice end")
{
  std::atomic<int> boundaries{0};
  hosted           h{
      alu(1'000'000),
      {.max_instructions = icount{10'000}, .wall_budget = long_wall},
      {.boundary = [&boundaries](core::member_id) {
        if(++boundaries == 4)
        {
          throw test_failure{"after the slice"};
        }
      }}
  };
  auto                     sentinel = std::make_shared<int>();
  std::weak_ptr<int> const watched  = sentinel;
  int                      calls    = 0;
  auto const               handle   = [&] {
    auto held = h.on->lock();
    h.on->start(held, 0);
    return h.on->wait(held, 0, {.ready = [sentinel, &calls]() -> bool {
                        ++calls;
                        return false;
                      }});
  }();
  sentinel.reset();
  CHECK_THROWS_AS(handle.future().get(), test_failure);
  REQUIRE(settles([&] { return parked_now(h); }));
  CHECK(calls == 1);
  CHECK(settles([&] { return watched.expired(); }, 500ms));
  auto const end = h.on->halt(0);
  REQUIRE(end.has_value());
  require_error(std::get<std::exception_ptr>(*end), [] { CHECK_THROWS_AS(throw, test_failure); });
}

TEST_CASE("halt after a free run ended by itself returns once the host released that turn")
{
  hosted h;
  end_by_handler(h);
  auto                             sentinel = std::make_shared<int>();
  std::weak_ptr<int> const         watched  = sentinel;
  std::atomic<bool>                stalled{false};
  std::atomic<bool>                resumed{false};
  core::host_future<popcorn::stop> pending;
  {
    auto held = h.on->lock();
    h.on->start(held, 0);
    pending = h.on->wait(held, 0, {.ready = [sentinel] { return false; }}).future();
    pending.on_done([&stalled, &resumed]() noexcept {
      stalled = true;
      std::this_thread::sleep_for(200ms);
      resumed = true;
    });
  }
  sentinel.reset();
  REQUIRE(settles([&] { return stalled.load(); }));
  auto const end = h.on->halt(0);
  CHECK(resumed);
  CHECK(watched.expired());
  REQUIRE(end.has_value());
  require_error(std::get<std::exception_ptr>(*end), [] { CHECK_THROWS_AS(throw, test_failure); });
}

TEST_CASE("a stop requested from another thread ends a free run with that stop")
{
  for(int round = 0; round < 20; ++round)
  {
    popcorn::testing::machine_fixture<stop_at_arch> f{m33(), "alu", core::machine_options{}};
    f.set("r0", 0);
    auto* const arch = f.module;
    hosted      h{std::move(f.target)};
    h.start();
    REQUIRE(settles([&] { return h.on->state(0) == core::host_state::running; }));
    arch->attached_services().request_stop(stop_reason::user, "from another thread");
    REQUIRE(settles([&] { return h.on->state(0) == core::host_state::parked; }));
    auto const end = h.on->halt(0);
    REQUIRE(end.has_value());
    auto const* const stopped = std::get_if<popcorn::stop>(&*end);
    REQUIRE(stopped != nullptr);
    CHECK(stopped->reason == stop_reason::user);
    CHECK(stopped->detail == "from another thread");
  }
}

TEST_CASE("a stop requested from another thread after a slice's last check stops the next slice")
{
  popcorn::testing::machine_fixture<stop_at_arch> f{m33(), "alu", core::machine_options{}};
  f.set("r0", 0);
  auto* const arch  = f.module;
  auto&       m     = *f.target;
  auto        armed = false;
  auto const  sink =
      m.add_trace_sink({core::trace_kind::block}, [&](std::span<core::trace_record const>) {
        if(std::exchange(armed, false))
        {
          std::thread{[arch] {
            arch->attached_services().request_stop(stop_reason::user, "late");
          }}.join();
        }
      });
  core::slice_request const bounded{.max_instructions = icount{100}, .wall_budget = long_wall};
  REQUIRE_FALSE(m.run_slice(bounded).stopped.has_value());
  armed            = true;
  auto const first = m.run_slice(bounded);
  CHECK_FALSE(armed);
  CHECK_FALSE(first.stopped.has_value());
  auto const second = m.run_slice(bounded);
  REQUIRE(second.stopped.has_value());
  CHECK(second.stopped->reason == stop_reason::user);
  CHECK(second.stopped->detail == "late");
  CHECK(second.executed == icount{0});
}
