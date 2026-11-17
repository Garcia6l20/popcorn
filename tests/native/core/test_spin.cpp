#include <popcorn/core/errors.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/core/trace.hpp>

#include "../../../native/src/core/machine_impl.hpp"
#include "../support/machine_fixture.hpp"
#include "../support/test_arch.hpp"
#include "../support/test_model.hpp"

#include <doctest/doctest.h>

#include <unicorn/unicorn.h>

#include <chrono>
#include <cstdint>
#include <fstream>
#include <limits>
#include <memory>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace
{
namespace core = popcorn::core;
using popcorn::address;
using popcorn::icount;
using popcorn::stop_reason;
using popcorn::testing::m33;
using popcorn::testing::sym;
using fixture = popcorn::testing::machine_fixture<>;

constexpr address flag = 0x4001'0008;

auto booted(std::uint32_t threshold) -> fixture
{
  core::machine_options options;
  options.spin_threshold = threshold;
  fixture f{m33(), "spin", options};
  REQUIRE(f.run_to_stop().reason == stop_reason::user);
  return f;
}

auto start(fixture const& f, std::string_view entry) -> void
{
  f.set("pc", sym("spin", entry));
}

auto long_request() -> core::slice_request
{
  core::slice_request request;
  request.max_instructions = icount{10'000'000};
  request.wall_budget      = std::chrono::seconds{60};
  return request;
}
}

TEST_CASE("a poll of an unmodelled status register stops as a spin naming it")
{
  auto f = booted(1000);
  start(f, "wait_ready");
  auto const before  = f.target->virtual_clock().now();
  auto const stopped = f.run_to_stop(long_request());
  CHECK(stopped.reason == stop_reason::spin);
  CAPTURE(stopped.detail);
  CHECK(stopped.detail.contains("in wait_ready"));
  CHECK(stopped.detail.contains("polling UNMODELLED.STAT = 0x00000001"));
  CHECK(stopped.detail.contains("fields READY"));
  auto const executed = f.target->virtual_clock().now().value - before.value;
  CHECK(executed == 4 + 3 * 1004);
}

TEST_CASE("a delay loop makes progress and never stops as a spin")
{
  auto f = booted(1000);
  start(f, "delay");
  CHECK(f.run_to_stop(long_request()).reason == stop_reason::user);
}

TEST_CASE("a poll of a flag set later stops only when the threshold is below the wait")
{
  constexpr std::uint64_t iterations = 40'000;
  auto const              wait       = [&](std::uint32_t threshold) {
    auto f = booted(threshold);
    start(f, "wait_flag");
    core::slice_request request = long_request();
    request.max_instructions    = icount{3 * iterations};
    auto const first            = f.run(request);
    if(first.stopped)
    {
      return first.stopped->reason;
    }
    REQUIRE(
        core::machine_internals::services(*f.target).write(
            flag, 4, 1, popcorn::initiator::debugger));
    return f.run_to_stop(long_request()).reason;
  };
  CHECK(wait(50'000) == stop_reason::user);
  CHECK(wait(10'000) == stop_reason::spin);
}

TEST_CASE("a loop storing a constant to RAM counts as no progress")
{
  auto f = booted(1000);
  start(f, "store_forever");
  auto const stopped = f.run_to_stop(long_request());
  CHECK(stopped.reason == stop_reason::spin);
  CHECK(stopped.detail.contains("in store_forever"));
  CHECK_FALSE(stopped.detail.contains("polling"));
}

TEST_CASE("a reset forgets the loop being tracked")
{
  auto f = booted(1000);
  start(f, "wait_ready");
  core::slice_request request = long_request();
  request.max_instructions    = icount{3 * 900};
  REQUIRE_FALSE(f.run(request).stopped.has_value());
  f.target->reset();
  REQUIRE(f.run_to_stop().reason == stop_reason::user);
  start(f, "wait_ready");
  request.max_instructions = icount{3 * 900};
  CHECK_FALSE(f.run(request).stopped.has_value());
  CHECK(f.run_to_stop(long_request()).reason == stop_reason::spin);
}

TEST_CASE("threshold 0 never tracks")
{
  auto f = booted(0);
  start(f, "store_forever");
  core::slice_request request = long_request();
  request.max_instructions    = icount{100'000};
  CHECK_FALSE(f.run(request).stopped.has_value());
}

namespace
{
auto resident_bytes() -> std::uint64_t
{
  std::ifstream statm{"/proc/self/statm"};
  std::uint64_t size     = 0;
  std::uint64_t resident = 0;
  statm >> size >> resident;
  return resident * 4096;
}
}

TEST_CASE("a loop longer than the window keeps its MMIO read list bounded")
{
  auto f = booted(1000);
  start(f, "long_poll");
  auto const before        = resident_bytes();
  auto       request       = long_request();
  request.max_instructions = icount{15'000'000};
  REQUIRE_FALSE(f.run(request).stopped.has_value());
  CHECK(resident_bytes() - before < 16u << 20);
}

TEST_CASE("an idle wait with a tick handler is not a spin")
{
  core::machine_options options;
  options.spin_threshold = 10;
  fixture f{m33(), "idle", options};
  f.set("pc", sym("idle", "idle_entry"));
  core::slice_request request{
      .wall_budget = std::chrono::seconds{60}, .until_time = popcorn::vtime{1'000'000'000'000}};
  for(;;)
  {
    auto const slice = f.run(request);
    if(slice.stopped)
    {
      CHECK(slice.stopped->reason == stop_reason::time);
      break;
    }
  }
}

TEST_CASE("a poll of a register whose model completes later is not a spin")
{
  core::machine_options options;
  options.spin_threshold = 1000;
  options.models.emplace("TESTDEV", popcorn::testing::test_model_factory());
  fixture f{m33(), "spin", options};
  REQUIRE(f.run_to_stop().reason == stop_reason::user);
  auto& model         = dynamic_cast<popcorn::testing::test_model&>(*f.target->model("TESTDEV"));
  model.go_delay      = 100'000;
  model.data_on_event = true;
  start(f, "wait_model");
  CHECK(f.run_to_stop(long_request()).reason == stop_reason::user);
  CHECK(model.fired_at.has_value());
}

TEST_CASE("a hung poll still spins while an interrupt handler reads MMIO")
{
  auto f = booted(1000);
  start(f, "wait_ready_ticking");
  auto const stopped = f.run_to_stop(long_request());
  CHECK(stopped.reason == stop_reason::spin);
  CAPTURE(stopped.detail);
  CHECK(stopped.detail.contains("polling UNMODELLED.STAT"));
}

TEST_CASE("reads made by a helper the loop calls belong to the iteration")
{
  SUBCASE("the helper's model completes later: no spin")
  {
    core::machine_options options;
    options.spin_threshold = 1000;
    options.models.emplace("TESTDEV", popcorn::testing::test_model_factory());
    fixture f{m33(), "spin", options};
    REQUIRE(f.run_to_stop().reason == stop_reason::user);
    auto& model         = dynamic_cast<popcorn::testing::test_model&>(*f.target->model("TESTDEV"));
    model.go_delay      = 100'000;
    model.data_on_event = true;
    start(f, "wait_helper");
    CHECK(f.run_to_stop(long_request()).reason == stop_reason::user);
  }
  SUBCASE("a hung helper poll names the register")
  {
    for(auto const* const entry : {"wait_helper_hung", "wait_near_helper"})
    {
      CAPTURE(entry);
      auto f = booted(1000);
      start(f, entry);
      auto const stopped = f.run_to_stop(long_request());
      CHECK(stopped.reason == stop_reason::spin);
      CAPTURE(stopped.detail);
      CHECK(stopped.detail.contains(std::string{"in "} + entry));
      CHECK(stopped.detail.contains("polling UNMODELLED.STAT"));
    }
  }
}

namespace
{
struct raw_pc_arch : popcorn::testing::test_arch
{
  using test_arch::test_arch;

  auto deliver_pending() -> popcorn::arch::trap_result override
  {
    inside            = true;
    auto const result = test_arch::deliver_pending();
    inside            = false;
    return result;
  }

  [[nodiscard]] auto pc() const -> address override
  {
    return inside ? test_arch::pc() : read(UC_ARM_REG_PC) & ~address{1};
  }

  bool inside = false;
};
}

TEST_CASE("plain WFI wakes inside a polling loop keep its MMIO reads counting")
{
  core::machine_options options;
  options.spin_threshold = 1000;
  SUBCASE("a hung poll names its register")
  {
    popcorn::testing::machine_fixture<raw_pc_arch> f{m33(), "spin", options};
    REQUIRE(f.run_to_stop().reason == stop_reason::user);
    f.set("pc", sym("spin", "wake_then_poll_hung"));
    auto const stopped = f.run_to_stop(long_request());
    CHECK(stopped.reason == stop_reason::spin);
    CAPTURE(stopped.detail);
    CHECK(stopped.detail.contains("polling UNMODELLED.STAT"));
  }
  SUBCASE("a model completing later restarts the count")
  {
    options.models.emplace("TESTDEV", popcorn::testing::test_model_factory());
    popcorn::testing::machine_fixture<raw_pc_arch> f{m33(), "spin", options};
    REQUIRE(f.run_to_stop().reason == stop_reason::user);
    auto& model         = dynamic_cast<popcorn::testing::test_model&>(*f.target->model("TESTDEV"));
    model.go_delay      = 100'000;
    model.data_on_event = true;
    f.set("pc", sym("spin", "wake_then_wait_model"));
    CHECK(f.run_to_stop(long_request()).reason == stop_reason::user);
  }
}

TEST_CASE("an inner poll that hangs inside an outer loop takes over as the loop")
{
  auto f = booted(1000);
  start(f, "nested_poll");
  auto const stopped = f.run_to_stop(long_request());
  CHECK(stopped.reason == stop_reason::spin);
  CAPTURE(stopped.detail);
  CHECK(stopped.detail.contains("in nested_poll"));
  CHECK(stopped.detail.contains("polling UNMODELLED.STAT"));
}

namespace
{
auto install_tick(fixture const& f, std::string_view handler) -> void
{
  REQUIRE(f.target->mem().write(0x0800'0000 + 4 * 16, 4, sym("spin", handler) | 1));
}
}

TEST_CASE("a loop in the tick handler does not restart the interrupted poll")
{
  auto f = booted(1000);
  install_tick(f, "tick_walk");
  start(f, "wait_ready_ticking");
  auto const stopped = f.run_to_stop(long_request());
  REQUIRE(stopped.reason == stop_reason::spin);
  CAPTURE(stopped.detail);
  CHECK(stopped.detail.contains("in wait_ready_ticking"));
  CHECK(stopped.detail.contains("polling UNMODELLED.STAT"));
}

TEST_CASE("a handler that hangs after an interrupted loop is a spin in the handler")
{
  auto f = booted(1000);
  install_tick(f, "tick_hang");
  start(f, "wait_ready_ticking");
  auto const stopped = f.run_to_stop(long_request());
  REQUIRE(stopped.reason == stop_reason::spin);
  CAPTURE(stopped.detail);
  CHECK(stopped.detail.contains("in tick_hang"));
  CHECK(stopped.detail.contains("polling UNMODELLED.STAT"));
}

TEST_CASE("calls stopped inside a handler leave the spin depth of their caller")
{
  auto f = booted(1000);
  install_tick(f, "tick_hang");
  popcorn::arch::call_frame const frame{
      .entry           = sym("spin", "wait_ready_ticking"),
      .return_sentinel = 0x3000'0000,
      .interrupts      = true,
  };
  auto const hang = sym("spin", "tick_hang") & ~address{1};
  for(std::uint32_t call = 0; call < popcorn::core::spin_state::max_levels + 1; ++call)
  {
    CAPTURE(call);
    auto const called = f.target->call(frame, icount{2000});
    REQUIRE(called.stopped.reason == stop_reason::budget);
    REQUIRE(called.stopped.where.pc >= hang);
    REQUIRE(called.stopped.where.pc < hang + 16);
  }
  install_tick(f, "tick_walk");
  start(f, "wait_ready_ticking");
  auto const stopped = f.run_to_stop(long_request());
  REQUIRE(stopped.reason == stop_reason::spin);
  CHECK(stopped.detail.contains("polling UNMODELLED.STAT"));
}

TEST_CASE("a masked call running another loop does not restart the paused poll")
{
  auto const uninterrupted = [] {
    auto f = booted(1000);
    start(f, "wait_ready");
    auto const slice = f.run(long_request());
    REQUIRE(slice.stopped.has_value());
    REQUIRE(slice.stopped->reason == stop_reason::spin);
    return slice.executed.value;
  }();
  auto f = booted(1000);
  start(f, "wait_ready");
  auto const first = f.run({.max_instructions = icount{1500}});
  REQUIRE_FALSE(first.stopped.has_value());
  popcorn::arch::call_frame const frame{
      .entry = sym("spin", "delay"), .return_sentinel = 0x3000'0000};
  REQUIRE(f.target->call(frame, icount{5000}).stopped.reason == stop_reason::budget);
  auto const rest = f.run(long_request());
  REQUIRE(rest.stopped.has_value());
  CHECK(rest.stopped->reason == stop_reason::spin);
  CHECK(first.executed.value + rest.executed.value == uninterrupted);
}

namespace
{
struct stamped
{
  icount             at{};
  core::trace_record record{};
};

struct loop_log
{
  std::vector<stamped> records{};
  icount               now{};

  [[nodiscard]] auto loops() const -> std::vector<stamped>
  {
    return records
         | std::views::filter(
               [](stamped const& entry) { return entry.record.kind == core::trace_kind::loop; })
         | std::ranges::to<std::vector>();
  }
};

auto listen(fixture const& f, core::trace_kinds kinds = {core::trace_kind::loop})
    -> std::pair<std::shared_ptr<loop_log>, core::hook_handle>
{
  auto log    = std::make_shared<loop_log>();
  log->now    = f.target->virtual_clock().now();
  auto handle = f.target->add_trace_sink(kinds, [log](std::span<core::trace_record const> batch) {
    for(auto const& record : batch)
    {
      if(record.kind == core::trace_kind::time)
      {
        log->now =
            icount{std::uint64_t{record.payload[0]} | std::uint64_t{record.payload[1]} << 32};
      }
      else
      {
        log->now.value += record.dt;
      }
      log->records.push_back({.at = log->now, .record = record});
    }
  });
  return {log, std::move(handle)};
}

auto head_of(std::string_view entry, address offset) -> address
{
  return (sym("spin", entry) & ~address{1}) + offset;
}

auto ender_of(core::trace_record const& record) -> core::loop_ender
{
  return static_cast<core::loop_ender>(record.aux >> core::trace_loop_ender_shift);
}

auto start_of(stamped const& entry) -> icount
{
  return icount{entry.at.value - entry.record.payload[2]};
}

auto budget(std::uint64_t instructions) -> core::slice_request
{
  core::slice_request request = long_request();
  request.max_instructions    = icount{instructions};
  return request;
}

void write_flag(fixture const& f, std::uint32_t value)
{
  REQUIRE(
      core::machine_internals::services(*f.target).write(
          flag, 4, value, popcorn::initiator::debugger));
}
}

TEST_CASE(
    "a fixed-point poll reports one run from its first identical pass, "
    "ended by turning reports off")
{
  auto f = booted(0);
  start(f, "wait_ready");
  auto [log, sink] = listen(f);
  f.target->set_loop_reports(1);
  auto const from = f.target->virtual_clock().now();
  REQUIRE_FALSE(f.run(budget(3001)).stopped.has_value());
  f.target->set_loop_reports(0);
  sink.reset();
  auto const loops = log->loops();
  REQUIRE(loops.size() == 1);
  auto const& run = loops.front();
  CHECK(run.record.payload[0] == head_of("wait_ready", 2));
  CHECK(run.record.flags == 0);
  CHECK(ender_of(run.record) == core::loop_ender::end);
  CHECK(run.at.value == from.value + 3001);
  CHECK(start_of(run).value == from.value + 13);
  CHECK(run.record.payload[1] == 995);
  CHECK(run.record.payload[2] == 3 * 995 + 3);
  CHECK(f.target->loop_reports() == 0);
}

TEST_CASE("loop reports never stop the run and a run below the report threshold writes nothing")
{
  auto f = booted(0);
  start(f, "wait_ready");
  {
    auto [log, sink] = listen(f);
    f.target->set_loop_reports(1000);
    CHECK_FALSE(f.run(budget(100'000)).stopped.has_value());
    f.target->set_loop_reports(0);
    sink.reset();
    REQUIRE(log->loops().size() == 1);
    CHECK(log->loops().front().record.payload[1] > 33'000);
  }
  start(f, "wait_ready");
  auto [log, sink] = listen(f);
  f.target->set_loop_reports(1000);
  REQUIRE_FALSE(f.run(budget(3 * 1000)).stopped.has_value());
  f.target->set_loop_reports(0);
  sink.reset();
  CHECK(log->loops().empty());
}

TEST_CASE("the spin stop and its instruction count are the same with loop reports on")
{
  for(auto const reports : {0u, 1u, 5000u})
  {
    CAPTURE(reports);
    auto f = booted(1000);
    start(f, "wait_ready");
    auto [log, sink] = listen(f);
    f.target->set_loop_reports(reports);
    auto const before  = f.target->virtual_clock().now();
    auto const stopped = f.run_to_stop(long_request());
    CHECK(stopped.reason == stop_reason::spin);
    CHECK(f.target->virtual_clock().now().value - before.value == 4 + 3 * 1004);
  }
}

TEST_CASE("a countdown makes progress every pass and reports no run")
{
  for(auto const* const entry : {"delay", "delay_r4"})
  {
    CAPTURE(entry);
    auto f = booted(0);
    start(f, entry);
    auto [log, sink] = listen(f);
    f.target->set_loop_reports(1);
    CHECK(f.run_to_stop(long_request()).reason == stop_reason::user);
    f.target->set_loop_reports(0);
    sink.reset();
    CHECK(log->loops().empty());
  }
}

TEST_CASE("a loop storing a constant to RAM each pass reports its passes")
{
  auto f = booted(0);
  start(f, "store_forever");
  auto [log, sink] = listen(f);
  f.target->set_loop_reports(1);
  REQUIRE_FALSE(f.run(budget(2000)).stopped.has_value());
  f.target->set_loop_reports(0);
  sink.reset();
  REQUIRE(log->loops().size() == 1);
  CHECK(log->loops().front().record.payload[0] == head_of("store_forever", 4));
  CHECK(log->loops().front().record.payload[1] > 900);
}

TEST_CASE("a poll whose read value changes ends its run there and starts another")
{
  auto f = booted(0);
  start(f, "poll_value");
  auto [log, sink] = listen(f);
  f.target->set_loop_reports(1);
  REQUIRE_FALSE(f.run(budget(3000)).stopped.has_value());
  auto const changed = f.target->virtual_clock().now();
  write_flag(f, 1);
  REQUIRE_FALSE(f.run(budget(3000)).stopped.has_value());
  f.target->set_loop_reports(0);
  sink.reset();
  auto const loops = log->loops();
  REQUIRE(loops.size() == 2);
  CHECK(ender_of(loops[0].record) == core::loop_ender::progress);
  CHECK(loops[0].at.value > changed.value);
  CHECK(loops[0].at.value - changed.value <= 3);
  CHECK(ender_of(loops[1].record) == core::loop_ender::end);
  CHECK(start_of(loops[1]).value == loops[0].at.value + 3);
  CHECK(loops[0].record.payload[0] == loops[1].record.payload[0]);
}

TEST_CASE("an exception entry ends the run at the entry and the next run starts after the return")
{
  auto f = booted(0);
  start(f, "wait_ready_ticking");
  auto [log, sink] = listen(f);
  f.target->set_loop_reports(1);
  REQUIRE_FALSE(f.run(budget(60'000)).stopped.has_value());
  f.target->set_loop_reports(0);
  sink.reset();
  auto const loops = log->loops();
  REQUIRE(loops.size() > 100);
  for(std::size_t at = 0; at + 1 < loops.size(); ++at)
  {
    CAPTURE(at);
    CHECK(ender_of(loops[at].record) == core::loop_ender::exception);
    CHECK((loops[at].record.aux & 0xfffu) == 0);
    CHECK(start_of(loops[at + 1]).value > loops[at].at.value);
    CHECK(loops[at].record.payload[2] < 500);
  }
  CHECK(ender_of(loops.back().record) == core::loop_ender::end);
}

TEST_CASE("a loop left for a sleep ends its run where the sleep starts")
{
  auto f = booted(0);
  start(f, "poll_then_sleep");
  auto [log, sink] = listen(f, {core::trace_kind::loop, core::trace_kind::sleep});
  f.target->set_loop_reports(1);
  REQUIRE_FALSE(f.run(budget(3000)).stopped.has_value());
  write_flag(f, 1);
  core::slice_request request{
      .wall_budget = std::chrono::seconds{60},
      .until_time  = popcorn::vtime{f.target->virtual_clock().time().ps + 1'000'000'000}};
  f.run(request);
  sink.reset();
  auto const& records = log->records;
  REQUIRE(records.size() == 2);
  CHECK(ender_of(records[0].record) == core::loop_ender::sleep);
  CHECK(records[1].record.kind == core::trace_kind::sleep);
  CHECK(records[1].at.value - records[1].record.payload[0] == records[0].at.value);
}

TEST_CASE("removing a sink writes the open run to it alone, and the run goes on for the others")
{
  auto f = booted(0);
  start(f, "wait_ready");
  auto [kept, kept_sink]       = listen(f);
  auto [removed, removed_sink] = listen(f);
  f.target->set_loop_reports(1);
  auto const from = f.target->virtual_clock().now();
  REQUIRE_FALSE(f.run(budget(1000)).stopped.has_value());
  removed_sink.reset();
  REQUIRE_FALSE(f.run(budget(1000)).stopped.has_value());
  auto const off = f.target->virtual_clock().now();
  f.target->set_loop_reports(0);
  kept_sink.reset();
  REQUIRE(removed->loops().size() == 1);
  REQUIRE(kept->loops().size() == 1);
  CHECK(ender_of(removed->loops().front().record) == core::loop_ender::end);
  CHECK(removed->loops().front().at.value == from.value + 1000);
  CHECK(start_of(removed->loops().front()).value == from.value + 13);
  CHECK(start_of(kept->loops().front()).value == from.value + 13);
  CHECK(kept->loops().front().at == off);
}

TEST_CASE("a reset or a restore ends the open run before the count moves")
{
  for(auto const restore : {false, true})
  {
    CAPTURE(restore);
    auto f = booted(0);
    start(f, "wait_ready");
    auto const saved = f.target->snapshot();
    auto [log, sink] = listen(f, {core::trace_kind::loop, core::trace_kind::time});
    f.target->set_loop_reports(1);
    auto const from = f.target->virtual_clock().now();
    REQUIRE_FALSE(f.run(budget(1000)).stopped.has_value());
    if(restore)
    {
      f.target->restore(saved);
    }
    else
    {
      f.target->reset();
    }
    f.target->set_loop_reports(0);
    sink.reset();
    auto const& records = log->records;
    REQUIRE(records.size() >= 2);
    CHECK(records[0].record.kind == core::trace_kind::loop);
    CHECK(ender_of(records[0].record) == core::loop_ender::rebase);
    CHECK(records[0].at.value == from.value + 1000);
    CHECK(start_of(records[0]).value == from.value + 13);
    CHECK(records[1].record.kind == core::trace_kind::time);
    CHECK((records[1].record.flags & core::trace_rebase_flag) != 0);
  }
}

TEST_CASE("a host call ends the open run at its start")
{
  auto f = booted(0);
  start(f, "wait_ready");
  auto [log, sink] = listen(f);
  f.target->set_loop_reports(1);
  auto const from = f.target->virtual_clock().now();
  REQUIRE_FALSE(f.run(budget(1000)).stopped.has_value());
  popcorn::arch::call_frame const frame{
      .entry = sym("spin", "delay"), .return_sentinel = 0x3000'0000};
  REQUIRE(f.target->call(frame, icount{50}).stopped.reason == stop_reason::budget);
  REQUIRE_FALSE(f.run(budget(1000)).stopped.has_value());
  f.target->set_loop_reports(0);
  sink.reset();
  auto const loops = log->loops();
  REQUIRE(loops.size() == 2);
  CHECK(ender_of(loops[0].record) == core::loop_ender::call);
  CHECK(loops[0].at.value == from.value + 1000);
  CHECK(start_of(loops[0]).value == from.value + 13);
  CHECK(start_of(loops[1]).value > from.value + 1050);
}

TEST_CASE("stepped slices stamp loop head arrivals at their block start")
{
  auto const exactly = [](fixture const& f, std::uint64_t instructions, std::uint64_t slice) {
    for(std::uint64_t done = 0; done < instructions; done += slice)
    {
      auto request               = long_request();
      request.exact_instructions = icount{std::min(slice, instructions - done)};
      auto const ran             = f.run(request);
      REQUIRE(ran.stopped.has_value());
      REQUIRE(ran.stopped->reason == stop_reason::budget);
    }
  };
  for(auto const slice : {7u, 300u})
  {
    CAPTURE(slice);
    {
      auto f = booted(0);
      start(f, "wait_ready");
      auto [log, sink] = listen(f);
      f.target->set_loop_reports(1);
      auto const from = f.target->virtual_clock().now();
      exactly(f, 3001, slice);
      f.target->set_loop_reports(0);
      sink.reset();
      auto const loops = log->loops();
      REQUIRE(loops.size() == 1);
      CHECK(loops.front().at.value == from.value + 3001);
      CHECK(start_of(loops.front()).value == from.value + 13);
      CHECK(loops.front().record.payload[1] == 995);
    }
    auto f = booted(0);
    start(f, "poll_value");
    auto [log, sink] = listen(f);
    f.target->set_loop_reports(1);
    exactly(f, 3000, slice);
    auto const changed = f.target->virtual_clock().now();
    write_flag(f, 1);
    exactly(f, 3000, slice);
    f.target->set_loop_reports(0);
    sink.reset();
    auto const loops = log->loops();
    REQUIRE(loops.size() == 2);
    CHECK(ender_of(loops[0].record) == core::loop_ender::progress);
    CHECK(loops[0].at.value == changed.value + 4);
    CHECK(start_of(loops[1]).value == loops[0].at.value + 3);
  }
}

TEST_CASE("a run inside a host call ends where the call stops, and the caller's run goes on after")
{
  auto f = booted(0);
  start(f, "wait_ready");
  auto [log, sink] = listen(f);
  f.target->set_loop_reports(1);
  auto const from = f.target->virtual_clock().now();
  REQUIRE_FALSE(f.run(budget(1000)).stopped.has_value());
  popcorn::arch::call_frame const frame{
      .entry = sym("spin", "wait_ready"), .return_sentinel = 0x3000'0000};
  REQUIRE(f.target->call(frame, icount{1000}).stopped.reason == stop_reason::budget);
  auto const stopped = f.target->virtual_clock().now();
  REQUIRE_FALSE(f.run(budget(1000)).stopped.has_value());
  f.target->set_loop_reports(0);
  sink.reset();
  auto const loops = log->loops();
  REQUIRE(loops.size() == 3);
  CHECK(ender_of(loops[0].record) == core::loop_ender::call);
  CHECK(loops[0].at.value == from.value + 1000);
  CHECK(ender_of(loops[1].record) == core::loop_ender::call);
  CHECK(loops[1].record.payload[0] == head_of("wait_ready", 2));
  CHECK(loops[1].at == stopped);
  CHECK(start_of(loops[1]).value > from.value + 1000);
  CHECK(loops[1].record.payload[1] > 300);
  CHECK(ender_of(loops[2].record) == core::loop_ender::end);
  CHECK(start_of(loops[2]).value >= stopped.value);
}

TEST_CASE("loop reports at spin_threshold 0 never stop a pass count at its largest value")
{
  auto f = booted(0);
  start(f, "wait_ready");
  f.target->set_loop_reports(1);
  REQUIRE_FALSE(f.run(budget(100)).stopped.has_value());
  auto& loop = core::machine_internals::spin(*f.target).loop();
  REQUIRE(loop.count > 20);
  loop.count = std::numeric_limits<std::uint32_t>::max() - 2;
  CHECK_FALSE(f.run(budget(30)).stopped.has_value());
  f.target->set_loop_reports(0);
}

TEST_CASE("a poll that drops the value it read ends its run when that value changes")
{
  auto f = booted(0);
  start(f, "poll_discard");
  auto [log, sink] = listen(f);
  f.target->set_loop_reports(1);
  REQUIRE_FALSE(f.run(budget(3000)).stopped.has_value());
  auto const changed = f.target->virtual_clock().now();
  write_flag(f, 1);
  REQUIRE_FALSE(f.run(budget(3000)).stopped.has_value());
  f.target->set_loop_reports(0);
  sink.reset();
  auto const loops = log->loops();
  REQUIRE(loops.size() == 2);
  CHECK(loops[0].record.payload[0] == head_of("poll_discard", 2));
  CHECK(ender_of(loops[0].record) == core::loop_ender::progress);
  CHECK(loops[0].at.value > changed.value);
  CHECK(loops[0].at.value - changed.value <= 3);
  CHECK(ender_of(loops[1].record) == core::loop_ender::end);
}

TEST_CASE("a poll left on its last pass reports the code from the exit to its ender in its run")
{
  auto f = booted(0);
  start(f, "poll_exit_tail");
  auto [log, sink] = listen(f, {core::trace_kind::loop, core::trace_kind::sleep});
  f.target->set_loop_reports(1);
  REQUIRE_FALSE(f.run(budget(3000)).stopped.has_value());
  write_flag(f, 1);
  core::slice_request request{
      .wall_budget = std::chrono::seconds{60},
      .until_time  = popcorn::vtime{f.target->virtual_clock().time().ps + 1'000'000'000}};
  f.run(request);
  sink.reset();
  auto const& records = log->records;
  REQUIRE(records.size() == 2);
  auto const& run = records[0];
  CHECK(ender_of(run.record) == core::loop_ender::sleep);
  CHECK(run.record.payload[0] == head_of("poll_exit_tail", 4));
  CHECK(run.record.payload[2] == 3 * run.record.payload[1] + 3 + 40 + 1);
  CHECK(records[1].record.kind == core::trace_kind::sleep);
  CHECK(records[1].at.value - records[1].record.payload[0] == run.at.value);
}

TEST_CASE("a loop whose last block starts 254 bytes past its head reports the offset in flags")
{
  for(auto const* const entry : {"widest_loop", "too_wide_loop"})
  {
    CAPTURE(entry);
    auto f = booted(0);
    start(f, entry);
    auto [log, sink] = listen(f);
    f.target->set_loop_reports(1);
    REQUIRE_FALSE(f.run(budget(1000)).stopped.has_value());
    f.target->set_loop_reports(0);
    sink.reset();
    if(std::string_view{entry} == "too_wide_loop")
    {
      CHECK(log->loops().empty());
      continue;
    }
    REQUIRE(log->loops().size() == 1);
    CHECK(log->loops().front().record.payload[0] == head_of(entry, 2));
    CHECK(log->loops().front().record.flags == 254);
  }
}

TEST_CASE("slice ends anywhere in a loop give the records of one slice")
{
  auto const records = [](std::uint64_t slice) {
    auto f = booted(0);
    start(f, "wait_ready_ticking");
    auto [log, sink] = listen(f);
    f.target->set_loop_reports(1);
    auto const target = f.target->virtual_clock().now().value + 6000;
    for(auto now = f.target->virtual_clock().now().value; now < target;
        now      = f.target->virtual_clock().now().value)
    {
      REQUIRE_FALSE(f.run(budget(std::min(slice, target - now))).stopped.has_value());
    }
    f.target->set_loop_reports(0);
    sink.reset();
    auto out = log->records
             | std::views::transform([](stamped const& entry) {
                 return std::tuple{entry.at.value,          entry.record.payload[0],
                                   entry.record.payload[1], entry.record.payload[2],
                                   entry.record.aux,        entry.record.flags};
               })
             | std::ranges::to<std::vector>();
    REQUIRE_FALSE(out.empty());
    auto const& last = out.back();
    out.back()       = {
        std::get<0>(last) - std::get<3>(last),
        std::get<1>(last),
        0,
        0,
        std::get<4>(last),
        std::get<5>(last)};
    return out;
  };
  auto const whole = records(6000);
  REQUIRE(whole.size() > 8);
  for(auto const slice : {1u, 2u, 7u, 499u, 1000u})
  {
    CAPTURE(slice);
    CHECK(records(slice) == whole);
  }
}

TEST_CASE("a run is reported from exactly the report threshold of passes")
{
  for(auto const& [threshold, reported] : {
          std::pair{995u, true },
          std::pair{996u, false}
  })
  {
    CAPTURE(threshold);
    auto f = booted(0);
    start(f, "wait_ready");
    auto [log, sink] = listen(f);
    f.target->set_loop_reports(threshold);
    REQUIRE_FALSE(f.run(budget(3001)).stopped.has_value());
    f.target->set_loop_reports(0);
    sink.reset();
    CHECK(log->loops().size() == (reported ? 1u : 0u));
  }
}

TEST_CASE("a handler loop left by its return ends its run at the return")
{
  auto f = booted(0);
  install_tick(f, "tick_wait_flag");
  start(f, "wait_ready_ticking");
  auto [log, sink] = listen(f);
  f.target->set_loop_reports(1);
  REQUIRE_FALSE(f.run(budget(3000)).stopped.has_value());
  write_flag(f, 1);
  REQUIRE_FALSE(f.run(budget(3000)).stopped.has_value());
  f.target->set_loop_reports(0);
  sink.reset();
  auto const loops = log->loops();
  auto const head  = head_of("tick_wait_flag", 2);
  auto const found = std::ranges::find_if(
      loops, [head](stamped const& entry) { return entry.record.payload[0] == head; });
  REQUIRE(found != loops.end());
  CHECK(ender_of(found->record) == core::loop_ender::progress);
  CHECK(found->record.payload[1] > 100);
}

namespace
{
auto stage_stale_candidate(fixture const& f) -> void
{
  start(f, "helper_below");
  auto const& loop = core::machine_internals::spin(*f.target).loop();
  for(auto step = 0; step < 50; ++step)
  {
    REQUIRE_FALSE(f.run(budget(1)).stopped.has_value());
    if(step > 20 and loop.candidate == head_of("helper_below", 8))
    {
      break;
    }
  }
  REQUIRE(loop.head == head_of("helper_below", 4));
  REQUIRE(loop.candidate == head_of("helper_below", 8));
  REQUIRE(loop.candidate_hits == 1);
  start(f, "wait_ready");
  REQUIRE_FALSE(f.run(budget(8)).stopped.has_value());
  REQUIRE(loop.head == head_of("wait_ready", 2));
  f.set("pc", head_of("helper_below", 4) | 1);
}
}

TEST_CASE("a loop with its helper below keeps the helper as head and counts its equal passes")
{
  auto f = booted(0);
  start(f, "helper_below");
  auto [log, sink] = listen(f);
  f.target->set_loop_reports(1);
  REQUIRE_FALSE(f.run(budget(5000)).stopped.has_value());
  f.target->set_loop_reports(0);
  sink.reset();
  auto const loops = log->loops();
  REQUIRE(loops.size() == 1);
  CHECK(loops.front().record.payload[0] == head_of("helper_below", 4));
  CHECK(loops.front().record.flags == 8);
  CHECK(loops.front().record.payload[1] > 990);
}

TEST_CASE(
    "a back edge taken under a former head does not make a loop with its helper below "
    "alternate heads")
{
  {
    auto f = booted(1000);
    stage_stale_candidate(f);
    auto const before  = f.target->virtual_clock().now();
    auto const stopped = f.run(budget(20'000));
    REQUIRE(stopped.stopped.has_value());
    CHECK(stopped.stopped->reason == stop_reason::spin);
    CAPTURE(stopped.stopped->detail);
    CHECK(stopped.stopped->detail.contains("in helper_below"));
    CHECK(f.target->virtual_clock().now().value - before.value == 5017);
  }
  auto f = booted(0);
  f.target->set_loop_reports(1);
  stage_stale_candidate(f);
  auto [log, sink] = listen(f);
  REQUIRE_FALSE(f.run(budget(5000)).stopped.has_value());
  f.target->set_loop_reports(0);
  sink.reset();
  auto const loops = log->loops()
                   | std::views::filter([](stamped const& entry) {
                       return entry.record.payload[0] != head_of("wait_ready", 2);
                     })
                   | std::ranges::to<std::vector>();
  REQUIRE(loops.size() == 1);
  CHECK(loops.front().record.payload[0] == head_of("helper_below", 4));
  CHECK(loops.front().record.payload[1] > 990);
}

TEST_CASE("a loop with its helper below ends its run when the helper reads another value")
{
  auto f = booted(0);
  start(f, "helper_below_flag");
  auto [log, sink] = listen(f);
  f.target->set_loop_reports(1);
  REQUIRE_FALSE(f.run(budget(3000)).stopped.has_value());
  write_flag(f, 1);
  REQUIRE_FALSE(f.run(budget(3000)).stopped.has_value());
  f.target->set_loop_reports(0);
  sink.reset();
  auto const loops = log->loops();
  REQUIRE(loops.size() == 2);
  CHECK(ender_of(loops[0].record) == core::loop_ender::progress);
  CHECK(ender_of(loops[1].record) == core::loop_ender::end);
  for(auto const& run : loops)
  {
    CHECK(run.record.payload[0] == head_of("helper_below_flag", 4));
  }
}

TEST_CASE("a poll left one pass short of the threshold into a countdown is no spin")
{
  constexpr std::uint32_t threshold = 50;
  auto                    f         = booted(threshold);
  start(f, "poll_then_countdown");
  auto const& loop = core::machine_internals::spin(*f.target).loop();
  REQUIRE_FALSE(f.run(budget(4)).stopped.has_value());
  while(loop.count < threshold - 2)
  {
    REQUIRE_FALSE(f.run(budget(3)).stopped.has_value());
  }
  REQUIRE(loop.count == threshold - 2);
  REQUIRE(loop.head == head_of("poll_then_countdown", 2));
  write_flag(f, 1);
  CHECK(f.run_to_stop(long_request()).reason == stop_reason::user);
}

TEST_CASE("a poll whose inner countdown repeats twice per pass is not seen: its head alternates")
{
  {
    auto f = booted(1000);
    start(f, "poll_with_inner_delay");
    CHECK_FALSE(f.run(budget(100'000)).stopped.has_value());
  }
  auto f = booted(0);
  start(f, "poll_with_inner_delay");
  auto [log, sink] = listen(f);
  f.target->set_loop_reports(1);
  REQUIRE_FALSE(f.run(budget(10'000)).stopped.has_value());
  auto const& loop = core::machine_internals::spin(*f.target).loop();
  CHECK(loop.count == 0);
  f.target->set_loop_reports(0);
  sink.reset();
  CHECK(log->loops().empty());
}

TEST_CASE("a poll whose inner countdown repeats once per pass from inside is seen")
{
  {
    auto f = booted(1000);
    start(f, "poll_with_short_inner_delay");
    auto const stopped = f.run(budget(100'000));
    REQUIRE(stopped.stopped.has_value());
    CHECK(stopped.stopped->reason == stop_reason::spin);
    CAPTURE(stopped.stopped->detail);
    CHECK(stopped.stopped->detail.contains("in poll_with_short_inner_delay"));
  }
  auto f = booted(0);
  start(f, "poll_with_short_inner_delay");
  auto [log, sink] = listen(f);
  f.target->set_loop_reports(1);
  REQUIRE_FALSE(f.run(budget(10'000)).stopped.has_value());
  f.target->set_loop_reports(0);
  sink.reset();
  auto const loops = log->loops();
  REQUIRE(loops.size() == 1);
  CHECK(loops.front().record.payload[0] == head_of("poll_with_short_inner_delay", 2));
  CHECK(loops.front().record.payload[1] > 900);
}
