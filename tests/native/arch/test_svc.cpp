#include "arch_fixture.hpp"

#include "cortex_m.hpp"

#include <popcorn/core/machine.hpp>
#include <popcorn/core/snapshot.hpp>

#include <doctest/doctest.h>

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace
{
namespace core = popcorn::core;
using popcorn::address;
using popcorn::stop_reason;
using popcorn::testing::arch_fixture;
using popcorn::testing::m33;
using popcorn::testing::sym;
using popcorn::testing::words_at;

constexpr address shcsr         = 0xE000'ED24;
constexpr address hfsr          = 0xE000'ED2C;
constexpr auto    svcall_pended = 1u << 15;

auto fn(std::string_view name) -> address
{
  return sym("svc", name);
}

auto code(std::string_view name) -> address
{
  return fn(name) & ~address{1};
}

auto booted(core::machine_options options = {}) -> arch_fixture
{
  arch_fixture f{m33(), "svc", std::move(options)};
  REQUIRE(f.run_to_stop().reason == stop_reason::user);
  f.set("pc", fn("svc_setup"));
  REQUIRE(f.run_to_stop().reason == stop_reason::user);
  return f;
}

auto log_of(arch_fixture const& f) -> std::vector<std::uint32_t>
{
  return words_at(*f.target, fn("log_buf"), f.word(fn("log_len")));
}

auto start(arch_fixture const& f, std::string_view entry) -> popcorn::stop
{
  f.set("pc", fn(entry));
  return f.run_to_stop();
}

auto registers_by_name(arch_fixture const& f) -> std::map<std::string, std::uint64_t>
{
  std::map<std::string, std::uint64_t> out;
  for(auto const& reg : f.module->registers())
  {
    out.emplace(reg.name, f.reg(reg.name));
  }
  return out;
}

void check_same_but_pc(
    std::map<std::string, std::uint64_t> before,
    std::map<std::string, std::uint64_t> after)
{
  before.erase("pc");
  after.erase("pc");
  for(auto const& [name, value] : before)
  {
    CAPTURE(name);
    CHECK(after.at(name) == value);
  }
}

auto psp_frame(arch_fixture const& f) -> std::uint32_t
{
  return f.word(fn("svc_psp_top")) - 0x20;
}

struct log_case
{
  std::string_view           entry;
  std::vector<std::uint32_t> expected;
};
}

TEST_CASE("each SVC, PendSV, NMI and STIR entry logs its sequence, twice in one process")
{
  auto const escalated = static_cast<std::uint32_t>(code("svc_escalate_at") + 2);
  for(int round = 0; round < 2; ++round)
  {
    CAPTURE(round);
    auto const                  top = psp_frame(booted());
    std::vector<log_case> const cases{
        {"svc_basic",        {0x805, 0x8FF}                },
        {"svc_psp",          {0x806, 0xFFFFFFFD, top}      },
        {"svc_handler_mode", {0x300, 0x807, 0x301}         },
        {"svc_escalate",     {0xC10, 0x40000000, escalated}},
        {"pendsv_basic",     {0xE00, 0xE14, 0xE01}         },
        {"pendsv_tail",      {0xE11, 0xE14, 0xE02}         },
        {"pendsv_clear",     {0xE03}                       },
        {"nmi_run",          {0xE1D, 0xE04}                },
        {"stir_run",         {0xE13, 0xE05}                },
        {"stir_unpriv",      {0xE06}                       },
        {"stir_allowed",     {0xE13, 0xE07}                },
    };
    for(auto const& one : cases)
    {
      CAPTURE(one.entry);
      auto f = booted();
      REQUIRE(start(f, one.entry).reason == stop_reason::user);
      CHECK(log_of(f) == one.expected);
      CHECK_FALSE(f.module->pending());
    }
  }
}

TEST_CASE("SVC from Thread enters SVCall before the next instruction and returns every register")
{
  auto f = booted();
  REQUIRE(start(f, "svc_regs").reason == stop_reason::user);
  auto const before = registers_by_name(f);
  REQUIRE(f.run_to_stop().reason == stop_reason::user);
  CHECK(f.reg("lr") == 0xFFFFFFF9);
  CHECK((f.reg("xpsr") & 0x1ff) == 11);
  auto const frame = words_at(*f.target, static_cast<address>(f.reg("msp")), 8);
  CHECK(frame[6] == code("svc_regs_at") + 2);
  REQUIRE(f.run_to_stop().reason == stop_reason::user);
  CHECK(f.reg("pc") == code("svc_regs_at") + 4);
  check_same_but_pc(before, registers_by_name(f));
}

TEST_CASE("SVC from unprivileged Thread on PSP keeps CONTROL and every register")
{
  auto f = booted();
  REQUIRE(start(f, "svc_unpriv").reason == stop_reason::user);
  auto const before = registers_by_name(f);
  REQUIRE(f.run_to_stop().reason == stop_reason::user);
  check_same_but_pc(before, registers_by_name(f));
  CHECK(f.reg("control") == 3);
  REQUIRE(f.run_to_stop().reason == stop_reason::user);
  auto const top = psp_frame(f);
  CHECK(log_of(f) == std::vector<std::uint32_t>{0x80A, 0xFFFFFFFD, 1, top, 0x8FE, 3});
}

TEST_CASE("an SVC that cannot preempt escalates to HardFault and returns past the svc")
{
  auto f = booted();
  REQUIRE(start(f, "svc_escalate").reason == stop_reason::user);
  CHECK(log_of(f)[1] == 0x40000000);
  CHECK(f.word(hfsr) == 0);
  CHECK((f.word(shcsr) & svcall_pended) == 0);
  CHECK_FALSE(f.module->pending());
}

TEST_CASE("an SVC under FAULTMASK is a lockup stop at the svc, again on the next slice")
{
  auto       f       = booted();
  auto const svc     = code("faultmask_svc_after") - 2;
  auto const stopped = start(f, "faultmask_svc");
  REQUIRE(stopped.reason == stop_reason::fault);
  REQUIRE(stopped.fault.has_value());
  CHECK(stopped.fault->kind == popcorn::fault_kind::hard);
  CHECK(stopped.fault->fault_address == svc);
  CHECK(stopped.where.pc == svc);
  CHECK_FALSE(f.module->pending());
  CHECK((f.word(shcsr) & svcall_pended) == 0);
  auto const again = f.run_to_stop();
  REQUIRE(again.reason == stop_reason::fault);
  CHECK(again.where.pc == svc);
  CHECK(again.fault->fault_address == svc);
  CHECK_FALSE(f.module->pending());
}

TEST_CASE("a stop right after an svc keeps SVCall pending through a serialized snapshot")
{
  auto       f     = booted();
  auto const after = code("svc_until_after");
  f.set("pc", fn("svc_until"));
  auto const stopped = f.run_to_stop({.until = after});
  REQUIRE(stopped.reason == stop_reason::address);
  CHECK(stopped.where.pc == after);
  CHECK((f.word(shcsr) & svcall_pended) != 0);
  auto const bytes = core::encode_snapshot(f.target->snapshot().data());

  arch_fixture other{m33(), "svc", {}};
  other.target->restore(core::snapshot{core::decode_snapshot(bytes)});
  REQUIRE(other.run_to_stop().reason == stop_reason::user);
  CHECK(log_of(other) == std::vector<std::uint32_t>{0x80C, static_cast<std::uint32_t>(after)});
  CHECK((other.word(shcsr) & svcall_pended) == 0);
}

TEST_CASE("an SVC-started two-task PendSV switch keeps each task's stack and FP state")
{
  auto f = booted();
  REQUIRE(start(f, "switch_run").reason == stop_reason::user);
  std::vector<std::uint32_t> expected;
  for(int index = 0; index < 5; ++index)
  {
    expected.push_back('A');
    expected.push_back('B');
  }
  CHECK(log_of(f) == expected);
  CHECK(f.word(fn("b_fp_seen")) == 0x44000000);
  auto const saved = words_at(*f.target, fn("task_sp"), 2);
  auto const first = words_at(*f.target, fn("task_sp_first"), 2);
  CHECK(saved[0] == first[0]);
  CHECK(saved[1] == first[1]);
  auto const control = words_at(*f.target, fn("control_seen"), 2);
  CHECK((control[0] & 0x4) == 0);
  CHECK((control[1] & 0x4) != 0);
  CHECK((f.reg("control") & 0x4) != 0);
}

TEST_CASE("a Thread poll loop that calls SVC is a spin on the polled register")
{
  core::machine_options options;
  options.spin_threshold = 1000;
  auto       f           = booted(std::move(options));
  auto const stopped     = start(f, "spin_svc");
  REQUIRE(stopped.reason == stop_reason::spin);
  CHECK(stopped.detail.find("UNMODELLED.STAT") != std::string::npos);
  CHECK(stopped.detail.find("TESTDEV") == std::string::npos);
}

TEST_CASE("a poll loop inside a handler that calls SVC is a spin once the depth has clamped at 0")
{
  core::machine_options options;
  options.spin_threshold = 1000;
  auto       f           = booted(std::move(options));
  auto const stopped     = start(f, "spin_svc_handler");
  REQUIRE(stopped.reason == stop_reason::spin);
  CHECK(stopped.detail.find("UNMODELLED.STAT") != std::string::npos);
  CHECK(stopped.detail.find("TESTDEV") == std::string::npos);
  CHECK((f.reg("xpsr") & 0x1ff) == 16 + 5);
}

TEST_CASE("coverage records no edge from the svc block into the SVCall handler")
{
  auto f = booted();
  f.target->enable_coverage({});
  REQUIRE(start(f, "svc_basic").reason == stop_reason::user);
  auto const handler = code("svc_entry");
  auto const data    = f.target->coverage();
  bool       entered = false;
  for(auto const& edge : data.edges)
  {
    CHECK(edge.to != handler);
    entered = entered or edge.from == handler;
  }
  CHECK(entered);
}
