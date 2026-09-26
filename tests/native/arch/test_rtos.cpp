#include "arch_fixture.hpp"

#include "../support/test_model.hpp"
#include "cortex_m.hpp"

#include <popcorn/core/clock.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/core/snapshot.hpp>

#include <reflex/serde/bson.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace
{
namespace core   = popcorn::core;
namespace arch   = popcorn::arch;
namespace armv8m = popcorn::arch::armv8m;
namespace periph = popcorn::periph;
using popcorn::address;
using popcorn::icount;
using popcorn::stop_reason;
using popcorn::vtime;
using popcorn::testing::arch_fixture;
using popcorn::testing::m33;
using popcorn::testing::sym;
using popcorn::testing::words_at;

constexpr std::uint64_t ps_per_ms   = 1'000'000'000;
constexpr std::uint32_t stack_bytes = 1024;
constexpr address       sentinel    = 0x3000'0000;

auto fn(std::string_view name) -> address
{
  return sym("rtos", name);
}

auto tag(char letter, std::uint32_t value) -> std::uint32_t
{
  return static_cast<std::uint32_t>(letter) << 24 | value;
}

struct rtos_machine
{
  std::shared_ptr<popcorn::testing::test_model> model{};
  std::unique_ptr<arch_fixture>                 f{};
  vtime                                         start{};
};

auto started(std::string_view entry, core::machine_options options = {}) -> rtos_machine
{
  rtos_machine out;
  options.models.emplace(
      "TESTDEV",
      [&model = out.model](periph::model_context& ctx, popcorn::peripheral_desc const&)
          -> std::shared_ptr<periph::model> {
        model = std::make_shared<popcorn::testing::test_model>(ctx);
        return model;
      });
  out.f = std::make_unique<arch_fixture>(m33(), "rtos", std::move(options));
  REQUIRE(out.f->run_to_stop().reason == stop_reason::user);
  out.start = out.f->target->virtual_clock().time();
  out.f->set("pc", fn(entry));
  return out;
}

auto at_ms(rtos_machine const& m, double ms) -> vtime
{
  return vtime{m.start.ps + static_cast<std::uint64_t>(ms * static_cast<double>(ps_per_ms))};
}

auto run_to(rtos_machine const& m, double ms) -> core::slice_result
{
  auto const result = m.f->run(
      {.max_instructions = icount{400'000'000},
       .wall_budget      = std::chrono::seconds{60},
       .until_time       = at_ms(m, ms)});
  REQUIRE(result.stopped.has_value());
  CHECK(result.stopped->reason == stop_reason::time);
  return result;
}

void run_to_task_a(rtos_machine const& m, double after_ms)
{
  run_to(m, after_ms);
  auto const result = m.f->run(
      {.max_instructions = icount{400'000'000},
       .wall_budget      = std::chrono::seconds{60},
       .until            = fn("task_a") & ~address{1}});
  REQUIRE(result.stopped.has_value());
  REQUIRE(result.stopped->reason == stop_reason::address);
}

auto log_of(rtos_machine const& m) -> std::vector<std::uint32_t>
{
  return words_at(*m.f->target, fn("log_buf"), m.f->word(fn("log_len")));
}

auto expected_log(std::uint32_t last_tick) -> std::vector<std::uint32_t>
{
  std::vector<std::uint32_t> out;
  for(std::uint32_t at = 1; at <= last_tick; ++at)
  {
    if(at % 3 == 0)
    {
      out.push_back(tag('A', at));
    }
    if(at % 5 == 0)
    {
      out.push_back(tag('B', at));
    }
  }
  return out;
}

auto registers_by_name(rtos_machine const& m) -> std::map<std::string, std::uint64_t>
{
  std::map<std::string, std::uint64_t> out;
  for(auto const& reg : m.f->module->registers())
  {
    out.emplace(reg.name, m.f->reg(reg.name));
  }
  return out;
}

auto live_special(rtos_machine const& m) -> armv8m::special_registers
{
  std::vector<std::byte> bytes;
  m.f->module->save(bytes);
  return reflex::serde::bson::deserializer{bytes}.load<armv8m::state>().special;
}

auto stack_base(std::uint32_t index) -> std::uint32_t
{
  return static_cast<std::uint32_t>(fn("task_stacks") + index * stack_bytes);
}

constexpr std::uint32_t task_a_index = 0;
constexpr std::uint32_t idle_index   = 1;
constexpr std::uint32_t task_b_index = 2;

struct outcome
{
  std::vector<std::uint32_t>           log;
  std::map<std::string, std::uint64_t> registers;
};

auto reference_run() -> outcome
{
  auto m = started("rtos_run");
  run_to(m, 30.5);
  return {log_of(m), registers_by_name(m)};
}
}

TEST_CASE("the RTOS starts from SVC, switches tasks through PendSV and logs in tick order, twice")
{
  std::vector<outcome> rounds;
  for(int round = 0; round < 2; ++round)
  {
    CAPTURE(round);
    auto m = started("rtos_run");
    run_to(m, 30.5);
    CHECK(log_of(m) == expected_log(30));
    CHECK(m.f->word(fn("fp_checks")) == 6);
    CHECK(m.f->word(fn("fp_bad")) == 0);
    for(auto const index : {task_a_index, task_b_index})
    {
      CAPTURE(index);
      CHECK(m.f->word(fn("psp_seen") + 4 * index) == stack_base(index) + stack_bytes);
      CHECK(m.f->word(fn("psp_bad") + 4 * index) == 0);
    }
    rounds.push_back({log_of(m), registers_by_name(m)});
  }
  CHECK(rounds[0].log == rounds[1].log);
  CHECK(rounds[0].registers == rounds[1].registers);
}

TEST_CASE("the idle RTOS runs 10 virtual seconds in under 1 % of the nominal instructions")
{
  auto       m      = started("rtos_idle");
  auto const before = std::chrono::steady_clock::now();
  auto const result = run_to(m, 10'000.5);
  auto const wall   = std::chrono::steady_clock::now() - before;
  MESSAGE(
      "10 virtual s: "
      << result.executed.value
      << " instructions, "
      << std::chrono::duration<double, std::milli>(wall).count()
      << " ms wall");
  CHECK(result.executed.value < 400'000);
  CHECK(m.f->word(fn("tick")) == 10'000);
}

TEST_CASE("an IRQ raised inside a BASEPRI critical section is taken after the section ends")
{
  auto m            = started("rtos_crit");
  m.model->go_delay = 100;
  run_to(m, 30.5);
  auto const log  = log_of(m);
  auto const find = [&](char letter) {
    return std::ranges::find_if(
        log, [&](std::uint32_t word) { return word >> 24 == static_cast<std::uint32_t>(letter); });
  };
  auto const section = find('S');
  auto const end     = find('E');
  auto const irq     = find('I');
  REQUIRE(section != log.end());
  REQUIRE(end != log.end());
  REQUIRE(irq != log.end());
  auto const pended = find('P');
  REQUIRE(pended != log.end());
  CHECK(section < end);
  CHECK(pended == end + 1);
  CHECK(*pended == tag('P', 1));
  CHECK(irq == pended + 1);
  CHECK(std::ranges::count_if(log, [](std::uint32_t word) { return word >> 24 == 'I'; }) == 1);
  std::vector<std::uint32_t> tasks_only;
  std::ranges::copy_if(log, std::back_inserter(tasks_only), [](std::uint32_t word) {
    return word >> 24 == 'A' or word >> 24 == 'B';
  });
  CHECK(tasks_only == expected_log(30));
}

TEST_CASE("a snapshot taken while task A runs restores to the same log and registers")
{
  auto m = started("rtos_run");
  run_to_task_a(m, 11.5);
  auto const taken = m.f->target->snapshot();
  run_to(m, 30.5);
  auto const log       = log_of(m);
  auto const registers = registers_by_name(m);
  m.f->target->restore(taken);
  run_to(m, 30.5);
  CHECK(log_of(m) == log);
  CHECK(registers_by_name(m) == registers);
  CHECK(log == expected_log(30));
}

TEST_CASE("a restore while another task runs brings task A's PSPLIM back, no STKOF")
{
  auto const reference = reference_run();
  for(auto const serialized : {true, false})
  {
    CAPTURE(serialized);
    auto m = started("rtos_run");
    run_to_task_a(m, 11.5);
    REQUIRE(live_special(m).psplim == stack_base(task_a_index));
    auto const taken = m.f->target->snapshot();
    auto const bytes = core::encode_snapshot(taken.data());
    run_to(m, 16.0);
    REQUIRE(live_special(m).psplim == stack_base(idle_index));
    if(serialized)
    {
      m.f->target->restore(core::snapshot{core::decode_snapshot(bytes)});
    }
    else
    {
      m.f->target->restore(taken);
    }
    CHECK(live_special(m).psplim == stack_base(task_a_index));
    run_to(m, 30.5);
    CHECK(log_of(m) == reference.log);
    CHECK(registers_by_name(m) == reference.registers);
  }
}

TEST_CASE("a masked call on the booted RTOS returns and leaves the task log unchanged")
{
  auto const reference = reference_run();
  auto       m         = started("rtos_run");
  run_to(m, 12.0);
  arch::call_frame const frame{
      .entry = fn("add"),
      .args =
          {{.kind = arch::value_kind::i32, .bits = 2}, {.kind = arch::value_kind::i32, .bits = 3}},
      .returns         = arch::value_kind::i32,
      .abi             = arch::float_abi::hard_double,
      .return_sentinel = sentinel
  };
  auto const called = m.f->target->call(frame, icount{100'000});
  CHECK(called.value == 5);
  run_to(m, 30.5);
  CHECK(log_of(m) == reference.log);
}

TEST_CASE("a task hung on a register after PendSV switches is a spin between two ticks")
{
  core::machine_options options;
  options.spin_threshold = 1000;
  auto       m           = started("rtos_hung", std::move(options));
  auto const stopped     = m.f->run_to_stop();
  REQUIRE(stopped.reason == stop_reason::spin);
  CHECK(stopped.detail.find("UNMODELLED.STAT") != std::string::npos);
  CHECK(m.f->word(fn("tick")) == 5);
  CHECK(log_of(m) == expected_log(5));
}

TEST_CASE("a task hung on a register spins across ticks whose handler walks a list")
{
  core::machine_options options;
  options.spin_threshold = 3000;
  auto       m           = started("rtos_hung", std::move(options));
  auto const result =
      m.f->run({.max_instructions = icount{1'000'000}, .wall_budget = std::chrono::seconds{60}});
  REQUIRE(result.stopped.has_value());
  CHECK(result.stopped->reason == stop_reason::spin);
  CHECK(result.stopped->detail.find("UNMODELLED.STAT") != std::string::npos);
  CHECK(m.f->word(fn("tick")) > 5);
}

TEST_CASE("the plain RTOS runs one virtual second without a spin stop")
{
  core::machine_options options;
  options.spin_threshold = 1000;
  auto m                 = started("rtos_run", std::move(options));
  run_to(m, 1000.5);
  CHECK(m.f->word(fn("tick")) == 1000);
  CHECK(m.f->word(fn("fp_bad")) == 0);
}
