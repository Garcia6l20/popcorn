#include "arch_fixture.hpp"

#include "../support/test_model.hpp"
#include "cortex_m.hpp"

#include <popcorn/core/machine.hpp>

#include <doctest/doctest.h>

#include <chrono>
#include <cstdint>
#include <format>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

namespace
{
namespace core   = popcorn::core;
namespace periph = popcorn::periph;
using popcorn::address;
using popcorn::stop_reason;
using popcorn::arch::fault_policy;
using popcorn::testing::arch_fixture;
using popcorn::testing::m33;
using popcorn::testing::sym;
using popcorn::testing::words_at;

auto fn(std::string_view name) -> address
{
  return sym("faults", name);
}

auto options_for(fault_policy policy, std::shared_ptr<popcorn::testing::test_model>& model)
    -> core::machine_options
{
  core::machine_options options;
  options.faults = policy;
  options.models.emplace(
      "TESTDEV",
      [&model](periph::model_context& ctx, popcorn::peripheral_desc const&)
          -> std::shared_ptr<periph::model> {
        model = std::make_shared<popcorn::testing::test_model>(ctx);
        return model;
      });
  return options;
}

auto log_of(arch_fixture const& f) -> std::vector<std::uint32_t>
{
  return words_at(*f.target, fn("log_buf"), f.word(fn("log_len")));
}

struct machine_under
{
  std::shared_ptr<popcorn::testing::test_model> model{};
  std::unique_ptr<arch_fixture>                 f{};
};

auto booted(fault_policy policy, core::machine_options extra = {}) -> machine_under
{
  machine_under out;
  auto          options  = options_for(policy, out.model);
  options.spin_threshold = extra.spin_threshold;
  out.f                  = std::make_unique<arch_fixture>(m33(), "faults", std::move(options));
  REQUIRE(out.f->run_to_stop().reason == stop_reason::user);
  out.f->set("pc", fn("faults_setup"));
  REQUIRE(out.f->run_to_stop().reason == stop_reason::user);
  return out;
}

auto run_entry(machine_under& m, std::string_view entry) -> popcorn::stop
{
  if(entry == "model_bus_error")
  {
    m.model->fail_writes = true;
  }
  m.f->set("pc", fn(entry));
  return m.f->run_to_stop();
}

struct handled_case
{
  std::string_view             entry;
  std::uint32_t                code;
  std::uint32_t                cfsr;
  std::optional<std::uint32_t> bfar;
  std::uint32_t                hfsr;
  address                      stacked_pc;
};

struct stopped_case
{
  std::string_view    entry;
  popcorn::fault_kind kind;
  address             fault_address;
  std::uint32_t       status;
  std::uint32_t       detail;
};
}

TEST_CASE("under enter_handler each fault enters its handler with the architectural registers")
{
  std::vector<handled_case> const cases{
      {"udf",                0xC40, 0x00010000, {},         0,          fn("udf_at")              },
      {"udf_forced",         0xC10, 0x00010000, {},         0x40000000, fn("udf_forced_at")       },
      {"unmapped_read",      0xC30, 0x00008200, 0x30000000, 0,          fn("unmapped_read_at")    },
      {"unmapped_write",     0xC30, 0x00008200, 0x30000000, 0,          fn("unmapped_write_at")   },
      {"unmapped_fetch",     0xC30, 0x00000100, {},         0,          0x30000000                },
      {"flash_write",        0xC30, 0x00008200, 0x08000400, 0,          fn("flash_write_at")      },
      {"model_bus_error",    0xC30, 0x00000400, {},         0,          fn("model_bus_error_next")},
      {"return_to_reset_lr", 0xC20, 0x00000001, {},         0,          0xFFFFFFFE                },
  };
  for(int round = 0; round < 2; ++round)
  {
    for(auto const& one : cases)
    {
      CAPTURE(round);
      CAPTURE(one.entry);
      auto m = booted(fault_policy::enter_handler);
      REQUIRE(run_entry(m, one.entry).reason == stop_reason::user);
      auto const log = log_of(*m.f);
      REQUIRE(log.size() == 5);
      CHECK(log[0] == one.code);
      CHECK(log[1] == one.cfsr);
      if(one.bfar)
      {
        CHECK(log[2] == *one.bfar);
      }
      CHECK(log[3] == one.hfsr);
      CHECK(log[4] == (one.stacked_pc & ~address{1}));
    }
  }
}

TEST_CASE("lockup and the entry and return errors stop under both policies")
{
  std::vector<stopped_case> const cases{
      {"faultmask_udf",        popcorn::fault_kind::hard,  fn("faultmask_udf") & ~address{1},   0,          0  },
      {"bad_exc_return",       popcorn::fault_kind::usage, 0xFFFFFFF5,                          0x00040000, 0  },
      {"stack_overflow_entry", popcorn::fault_kind::bus,   0x300000E0,                          0x00001000, 0  },
      {"bad_vector",           popcorn::fault_kind::hard,  0x30000040,                          0,          0x2},
      {"invstate",             popcorn::fault_kind::usage, fn("invstate_target") & ~address{1}, 0x00020000, 0  },
  };
  for(auto const policy : {fault_policy::enter_handler, fault_policy::stop})
  {
    for(auto const& one : cases)
    {
      CAPTURE(policy);
      CAPTURE(one.entry);
      auto       m       = booted(policy);
      auto const stopped = run_entry(m, one.entry);
      REQUIRE(stopped.reason == stop_reason::fault);
      REQUIRE(stopped.fault.has_value());
      CHECK(stopped.fault->kind == one.kind);
      if(one.entry != "faultmask_udf")
      {
        CHECK(stopped.fault->fault_address == one.fault_address);
        CHECK(stopped.fault->status == one.status);
        CHECK(stopped.fault->detail == one.detail);
      }
      auto const failed_entry = one.entry == "stack_overflow_entry" or one.entry == "bad_vector";
      CHECK(m.f->module->pending() == failed_entry);
    }
  }
}

TEST_CASE("a fault in the HardFault handler is a lockup, a UDF under FAULTMASK too")
{
  {
    auto       m       = booted(fault_policy::enter_handler);
    auto const stopped = run_entry(m, "fault_in_hardfault");
    REQUIRE(stopped.reason == stop_reason::fault);
    CHECK(stopped.fault->kind == popcorn::fault_kind::hard);
    CHECK((m.f->reg("xpsr") & 0x1ff) == 3);
    CHECK(stopped.fault->detail == 0x40000000);
  }
  {
    auto       m       = booted(fault_policy::stop);
    auto const stopped = run_entry(m, "fault_in_hardfault");
    REQUIRE(stopped.reason == stop_reason::fault);
    CHECK(stopped.fault->kind == popcorn::fault_kind::usage);
    CHECK(stopped.fault->detail == 0x40000000);
  }
  for(auto const policy : {fault_policy::enter_handler, fault_policy::stop})
  {
    CAPTURE(policy);
    auto       m       = booted(policy);
    auto const stopped = run_entry(m, "faultmask_udf");
    REQUIRE(stopped.reason == stop_reason::fault);
    CHECK(stopped.fault->kind == popcorn::fault_kind::hard);
    CHECK(stopped.where.pc == stopped.fault->fault_address);
  }
}

TEST_CASE("under the stop policy a fault reports its class, address and CFSR, and stops again")
{
  {
    auto       m       = booted(fault_policy::stop);
    auto const udf     = fn("udf_at") & ~address{1};
    auto const stopped = run_entry(m, "udf");
    REQUIRE(stopped.reason == stop_reason::fault);
    CHECK(stopped.fault->kind == popcorn::fault_kind::usage);
    CHECK(stopped.fault->fault_address == udf);
    CHECK(stopped.where.pc == udf);
    CHECK(stopped.fault->status == 0x00010000);
    auto const again = m.f->run_to_stop();
    REQUIRE(again.reason == stop_reason::fault);
    CHECK(again.where.pc == udf);
    CHECK_FALSE(m.f->module->pending());
  }
  {
    auto       m       = booted(fault_policy::stop);
    auto const stopped = run_entry(m, "unmapped_read");
    REQUIRE(stopped.reason == stop_reason::fault);
    CHECK(stopped.fault->kind == popcorn::fault_kind::bus);
    CHECK(stopped.fault->fault_address == 0x30000000);
    CHECK(stopped.fault->status == 0x00008200);
    CHECK(stopped.where.pc == (fn("unmapped_read_at") & ~address{1}));
    CHECK(m.f->target->mem().read(0xE000'ED28, 4) == 0x00008200u);
    CHECK(m.f->target->mem().read(0xE000'ED38, 4) == 0x30000000u);
  }
}

TEST_CASE("a UDF polled in a loop with a UsageFault handler reading a register is a spin")
{
  core::machine_options options;
  options.spin_threshold = 1000;
  auto       m           = booted(fault_policy::enter_handler, std::move(options));
  auto const stopped     = run_entry(m, "spin_udf");
  REQUIRE(stopped.reason == stop_reason::spin);
  CHECK(stopped.detail.find("UNMODELLED.STAT") != std::string::npos);
  CHECK(stopped.detail.find("TESTDEV") == std::string::npos);
}

TEST_CASE("recorded: MSPLIM raises STKOF, and a reset clears MSPLIM")
{
  {
    auto       m       = booted(fault_policy::stop);
    auto const stopped = run_entry(m, "stack_limit");
    REQUIRE(stopped.reason == stop_reason::fault);
    CHECK(stopped.fault->kind == popcorn::fault_kind::usage);
    CHECK(stopped.fault->status == 0x00100000);
  }
  {
    auto       m       = booted(fault_policy::enter_handler);
    auto const stopped = run_entry(m, "stack_limit");
    REQUIRE(stopped.reason == stop_reason::fault);
    CHECK(stopped.fault->kind == popcorn::fault_kind::hard);
    CHECK((stopped.fault->status & 0x00100000) != 0);
    m.f->target->reset();
    REQUIRE(m.f->run_to_stop().reason == stop_reason::user);
    m.f->set("pc", fn("deep_stack_run"));
    CHECK(m.f->run_to_stop().reason == stop_reason::user);
  }
}

TEST_CASE("recorded: an unprivileged read of CPUID is answered, no BusFault")
{
  auto m = booted(fault_policy::enter_handler);
  REQUIRE(run_entry(m, "unpriv_scs").reason == stop_reason::user);
  CHECK(m.f->word(fn("unpriv_read")) == m.f->module->cpuid());
  CHECK(log_of(*m.f).empty());
}
