#include <popcorn/core/errors.hpp>
#include <popcorn/core/host.hpp>
#include <popcorn/core/machine.hpp>

#include "../support/machine_fixture.hpp"
#include "../support/test_arch.hpp"

#include <doctest/doctest.h>
#include <pthread.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <exception>
#include <functional>
#include <latch>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <thread>
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

  auto post(std::move_only_function<void(core::machine&)> body) const -> core::host_future<vtime>
  {
    auto held = on->lock();
    return on->post(held, 0, std::move(body));
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
