#include "arch_fixture.hpp"

#include "../support/test_model.hpp"
#include "cortex_m.hpp"
#include "exceptions.hpp"
#include "state.hpp"

#include <popcorn/core/machine.hpp>

#include <doctest/doctest.h>
#include <reflex/serde/bson.hpp>

#include <chrono>
#include <cstddef>
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
using popcorn::testing::m33_trustzone;
using popcorn::testing::trustzone_options;
namespace armv8m = popcorn::arch::armv8m;
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

auto booted_trustzone(fault_policy policy) -> machine_under
{
  machine_under out;
  auto          options = options_for(policy, out.model);
  auto const    secure  = trustzone_options();
  options.trustzone     = secure.trustzone;
  options.idau          = secure.idau;
  out.f = std::make_unique<arch_fixture>(m33_trustzone(), "faults", std::move(options));
  REQUIRE(out.f->run_to_stop().reason == stop_reason::user);
  out.f->set("pc", fn("faults_setup"));
  REQUIRE(out.f->run_to_stop().reason == stop_reason::user);
  return out;
}

auto deferred_of(machine_under const& m) -> std::optional<armv8m::deferred_fault>
{
  std::vector<std::byte> bytes;
  m.f->module->save(bytes);
  return reflex::serde::bson::deserializer{bytes}.load<armv8m::state>().deferred;
}

void set_bfhfnmins(machine_under const& m)
{
  REQUIRE(m.f->target->mem().write(0xE000'ED0C, 4, 0x05FA'0000u | (1u << 13)));
}

auto run_entry(machine_under& m, std::string_view entry) -> popcorn::stop
{
  if(entry == "model_bus_error" or entry.starts_with("masked_bus_error"))
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

TEST_CASE(
    "a return from the HardFault handler with a forged IPSR clears HardFault, "
    "DeActivate lines 10-12")
{
  auto m = booted(fault_policy::enter_handler);
  REQUIRE(run_entry(m, "udf_forced").reason == stop_reason::user);
  REQUIRE((m.f->reg("xpsr") & 0x1ff) == armv8m::exc_hardfault);
  REQUIRE(m.f->module->is_active(armv8m::exc_hardfault));
  auto const             stacked_pc = fn("udf_forced_at") & ~address{1};
  auto const             sp         = m.f->reg("sp");
  std::optional<address> slot;
  for(address at = sp; at < sp + 80 and not slot; at += 4)
  {
    auto const word = m.f->target->mem().read(at, 4);
    if(word and *word == stacked_pc)
    {
      slot = at;
    }
  }
  REQUIRE(slot.has_value());
  REQUIRE(m.f->target->mem().write(*slot, 4, fn("faults_setup") | 1u));
  m.f->set("xpsr", (m.f->reg("xpsr") & ~std::uint64_t{0x1ff}) | (16 + 11));
  REQUIRE(m.f->run_to_stop().reason == stop_reason::user);
  CHECK((m.f->reg("xpsr") & 0x1ff) == 0);
  CHECK_FALSE(m.f->module->is_active(armv8m::exc_hardfault));
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
  {
    auto       m       = booted(fault_policy::stop);
    auto const stopped = run_entry(m, "model_bus_error");
    REQUIRE(stopped.reason == stop_reason::fault);
    CHECK(stopped.fault->kind == popcorn::fault_kind::bus);
    CHECK(stopped.fault->fault_address == 0x40010058);
    CHECK(stopped.fault->status == 0x00000400);
    CHECK(stopped.fault->detail == 0);
    CHECK(stopped.where.pc == (fn("model_bus_error_next") & ~address{1}));
    CHECK_FALSE(m.f->module->pending());
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

TEST_CASE("MSPLIM raises STKOF, the UsageFault entry below it stops, a reset clears MSPLIM")
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
    CHECK(stopped.fault->kind == popcorn::fault_kind::usage);
    CHECK(stopped.fault->status == 0x00100000);
    CHECK(m.f->module->is_pending(6));
    m.f->target->reset();
    REQUIRE(m.f->run_to_stop().reason == stop_reason::user);
    m.f->set("pc", fn("deep_stack_run"));
    CHECK(m.f->run_to_stop().reason == stop_reason::user);
  }
}

TEST_CASE(
    "an unprivileged read of CPUID takes a precise BusFault and reads nothing, "
    "B8.2 RRTKK and RNDML")
{
  auto m = booted(fault_policy::enter_handler);
  REQUIRE(run_entry(m, "unpriv_scs").reason == stop_reason::user);
  CHECK(m.f->word(fn("unpriv_read")) == 0);
  auto const log = log_of(*m.f);
  REQUIRE(log.size() == 5);
  CHECK(log[0] == 0xC30);
  CHECK(log[1] == 0x00008200);
  CHECK(log[2] == 0xE000'ED00);
  CHECK(log[3] == 0);
}

namespace
{
constexpr address       unprivileged_code = 0x2008'0000;
constexpr address       shcsr_at          = 0xE000'ED24;
constexpr std::uint32_t busfault_ena      = 1u << 17;
constexpr std::uint16_t ldr_r0_r3         = 0x6818;
constexpr std::uint16_t str_r1_r3         = 0x6019;
constexpr std::uint16_t bkpt_1            = 0xbe01;

struct unprivileged_access
{
  std::string_view title;
  address          at;
  bool             write;
  std::uint32_t    value;
};

void run_unprivileged(machine_under& m, unprivileged_access const& access)
{
  address cursor = unprivileged_code;
  for(auto const half : {access.write ? str_r1_r3 : ldr_r0_r3, bkpt_1})
  {
    REQUIRE(m.f->target->mem().write(cursor, 2, half));
    cursor += 2;
  }
  m.f->set("r1", access.value);
  m.f->set("r3", access.at);
  m.f->set("control", 1);
  m.f->set("pc", unprivileged_code | 1);
}
}

TEST_CASE(
    "an unprivileged access to a system space register takes a precise BusFault and changes "
    "nothing, escalated to HardFault with BusFault disabled, B8.2 RRTKK and RNSWG")
{
  std::vector<unprivileged_access> const accesses{
      {"CPUID read",       0xE000'ED00, false, 0          },
      {"NVIC_IPR0 read",   0xE000'E400, false, 0          },
      {"NVIC_IPR0 write",  0xE000'E400, true,  0xff       },
      {"AIRCR write",      0xE000'ED0C, true,  0x05FA'0300},
      {"VTOR write",       0xE000'ED08, true,  0x2000'0080},
      {"SYST_CSR write",   0xE000'E010, true,  7          },
      {"DWT_CTRL read",    0xE000'1000, false, 0          },
      {"FP_CTRL read",     0xE000'2000, false, 0          },
      {"DWT offset 0xF00", 0xE000'1F00, false, 0          },
      {"FPB offset 0xF00", 0xE000'2F00, false, 0          },
      {"ITM offset 0xF00", 0xE000'0F00, false, 0          },
      {"ITM offset 0x400", 0xE000'0400, true,  1          },
  };
  for(auto const enabled : {true, false})
  {
    for(auto const& access : accesses)
    {
      CAPTURE(enabled);
      CAPTURE(access.title);
      auto m = booted(fault_policy::enter_handler);
      REQUIRE(
          m.f->target->mem().write(0xE000'ED14, 4, *m.f->target->mem().read(0xE000'ED14, 4) | 2u));
      if(enabled)
      {
        REQUIRE(m.f->target->mem().write(
            shcsr_at, 4, *m.f->target->mem().read(shcsr_at, 4) | busfault_ena));
      }
      auto const before = m.f->target->mem().read(access.at, 4);
      REQUIRE(before.has_value());
      run_unprivileged(m, access);
      REQUIRE(m.f->run_to_stop().reason == stop_reason::user);
      auto const log = log_of(*m.f);
      REQUIRE(log.size() == 5);
      CHECK(log[0] == (enabled ? 0xC30u : 0xC10u));
      CHECK(log[1] == 0x00008200);
      CHECK(log[2] == access.at);
      CHECK(log[3] == (enabled ? 0u : 0x4000'0000u));
      CHECK((m.f->reg("xpsr") & 0x1ff) == (enabled ? armv8m::exc_busfault : armv8m::exc_hardfault));
      CHECK(m.f->target->mem().read(access.at, 4) == before);
    }
  }
}

TEST_CASE(
    "CCR.BFHFNMIGN with a negative execution priority skips an unprivileged system space access "
    "and records it in CFSR and BFAR")
{
  auto m = booted(fault_policy::enter_handler);
  REQUIRE(
      m.f->target->mem().write(shcsr_at, 4, *m.f->target->mem().read(shcsr_at, 4) | busfault_ena));
  REQUIRE(m.f->target->mem().write(
      0xE000'ED14, 4, *m.f->target->mem().read(0xE000'ED14, 4) | (1u << 8)));
  m.f->set("faultmask", 1);
  m.f->set("r0", 0xA5A5);
  run_unprivileged(m, {"CPUID read", 0xE000'ED00, false, 0});
  REQUIRE(m.f->run_to_stop().reason == stop_reason::user);
  CHECK(m.f->reg("r0") == 0xA5A5);
  CHECK(log_of(*m.f).empty());
  CHECK((m.f->word(0xE000'ED28) & 0x8200) == 0x8200);
  CHECK(m.f->word(0xE000'ED38) == 0xE000'ED00);
}

TEST_CASE(
    "an unprivileged write to STIR pends the interrupt with CCR.USERSETMPEND set and takes a "
    "BusFault with it clear, B8.2 RDQQS")
{
  constexpr address ccr = 0xE000'ED14;
  for(auto const allowed : {true, false})
  {
    CAPTURE(allowed);
    auto m = booted(fault_policy::enter_handler);
    REQUIRE(m.f->target->mem().write(
        shcsr_at, 4, *m.f->target->mem().read(shcsr_at, 4) | busfault_ena));
    REQUIRE(
        m.f->target->mem().write(ccr, 4, *m.f->target->mem().read(ccr, 4) | (allowed ? 2u : 0u)));
    m.f->set("primask", 1);
    run_unprivileged(m, {"STIR write", 0xE000'EF00, true, 0});
    REQUIRE(m.f->run_to_stop().reason == stop_reason::user);
    CHECK(m.f->module->is_pending(16) == allowed);
    CHECK(log_of(*m.f).empty() == allowed);
  }
}

TEST_CASE("exception entry stacking below the pushed stack's limit stops with STKOF")
{
  struct stkof_case
  {
    std::string_view entry;
    bool             overflows;
    bool             process;
    std::uint32_t    size;
    std::uint32_t    pending;
  };
  std::vector<stkof_case> const cases{
      {"stkof_main_fits",      false, false, 0x20, 14},
      {"stkof_main_basic",     true,  false, 0x20, 14},
      {"stkof_main_fp_fits",   false, false, 0x68, 14},
      {"stkof_main_fp",        true,  false, 0x68, 14},
      {"stkof_process_basic",  true,  true,  0x20, 14},
      {"stkof_process_msplim", false, true,  0x20, 14},
      {"stkof_handler",        true,  false, 0x20, 2 },
  };
  for(auto const policy : {fault_policy::enter_handler, fault_policy::stop})
  {
    for(auto const& one : cases)
    {
      CAPTURE(policy);
      CAPTURE(one.entry);
      auto       m       = booted(policy);
      auto const stopped = run_entry(m, one.entry);
      if(not one.overflows)
      {
        CHECK(stopped.reason == stop_reason::user);
        CHECK(m.f->reg("xpsr") % 0x200 == one.pending);
        CHECK_FALSE(m.f->module->is_pending(one.pending));
        continue;
      }
      REQUIRE(stopped.reason == stop_reason::fault);
      REQUIRE(stopped.fault.has_value());
      auto const sp = static_cast<address>(m.f->reg(one.process ? "psp" : "msp"));
      CHECK(stopped.fault->kind == popcorn::fault_kind::usage);
      CHECK(stopped.fault->fault_address == ((sp - one.size) & ~address{7}));
      CHECK(stopped.fault->status == 0x00100000);
      CHECK(stopped.fault->detail == 0);
      CHECK(m.f->module->is_pending(one.pending));
      CHECK_FALSE(m.f->module->is_active(one.pending));
    }
  }
}

TEST_CASE("an imprecise BusFault under FAULTMASK cleared through SHCSR is never taken")
{
  for(auto const policy : {fault_policy::enter_handler, fault_policy::stop})
  {
    CAPTURE(policy);
    auto m = booted(policy);
    REQUIRE(run_entry(m, "masked_bus_error_cleared").reason == stop_reason::user);
    auto const log = log_of(*m.f);
    REQUIRE(log.size() == 4);
    CHECK(log[0] == 0x00000400);
    CHECK((log[1] & 0x00024000) == 0x00024000);
    CHECK(log[2] == 0);
    CHECK((log[3] & 0x00024000) == 0);
    CHECK_FALSE(m.f->module->pending());
    CHECK((m.f->reg("xpsr") & 0x1ff) == 0);
    REQUIRE(m.f->target->mem().write(0xE000'ED24, 4, 0x00024000));
    m.f->set("pc", fn("deep_stack_run"));
    REQUIRE(m.f->run_to_stop().reason == stop_reason::user);
    CHECK((m.f->reg("xpsr") & 0x1ff) == 5);
    auto const handled = log_of(*m.f);
    REQUIRE(handled.size() == 9);
    CHECK(handled[4] == 0xC30);
  }
}

TEST_CASE("an imprecise BusFault pended under FAULTMASK stops at its entry under the stop policy")
{
  struct released_case
  {
    std::string_view entry;
    std::string_view release_at;
    std::uint32_t    hfsr;
  };
  std::vector<released_case> const cases{
      {"masked_bus_error_released", "masked_release_at", 0         },
      {"masked_bus_error_forced",   "masked_forced_at",  0x40000000},
  };
  for(auto const& one : cases)
  {
    CAPTURE(one.entry);
    auto m = booted(fault_policy::stop);
    REQUIRE(run_entry(m, one.entry).reason == stop_reason::user);
    CHECK(m.f->module->pending());
    CHECK(((log_of(*m.f)[1] >> 14) & 1u) == (one.hfsr == 0 ? 1u : 0u));
    auto const taken = m.f->target->snapshot();
    for(int round = 0; round < 2; ++round)
    {
      CAPTURE(round);
      if(round == 1)
      {
        m.f->target->restore(taken);
      }
      auto const stopped = m.f->run_to_stop();
      REQUIRE(stopped.reason == stop_reason::fault);
      REQUIRE(stopped.fault.has_value());
      CHECK(stopped.fault->kind == popcorn::fault_kind::bus);
      CHECK(stopped.fault->fault_address == 0x40010058);
      CHECK(stopped.fault->status == 0x00000400);
      CHECK(stopped.fault->detail == one.hfsr);
      CHECK(stopped.where.pc == (fn(one.release_at) & ~address{1}));
      CHECK((m.f->reg("xpsr") & 0x1ff) == 0);
      CHECK_FALSE(m.f->module->pending());
      CHECK(log_of(*m.f).size() == 2);
      auto const again = m.f->run_to_stop();
      CHECK(again.reason == stop_reason::user);
      CHECK(log_of(*m.f).size() == 2);
    }
  }
}

TEST_CASE("an imprecise BusFault pended under FAULTMASK enters its handler at the release")
{
  struct released_case
  {
    std::string_view entry;
    std::string_view release_at;
    std::uint32_t    code;
    std::uint32_t    ipsr;
    std::uint32_t    hfsr;
  };
  std::vector<released_case> const cases{
      {"masked_bus_error_released", "masked_release_at", 0xC30, 5, 0         },
      {"masked_bus_error_forced",   "masked_forced_at",  0xC10, 3, 0x40000000},
  };
  for(auto const& one : cases)
  {
    CAPTURE(one.entry);
    auto m = booted(fault_policy::enter_handler);
    REQUIRE(run_entry(m, one.entry).reason == stop_reason::user);
    REQUIRE(m.f->run_to_stop().reason == stop_reason::user);
    CHECK((m.f->reg("xpsr") & 0x1ff) == one.ipsr);
    auto const log = log_of(*m.f);
    REQUIRE(log.size() == 7);
    CHECK(log[0] == 0x00000400);
    CHECK(log[2] == one.code);
    CHECK(log[3] == 0x00000400);
    CHECK(log[5] == one.hfsr);
    CHECK(log[6] == (fn(one.release_at) & ~address{1}));
  }
}

TEST_CASE("a second masked imprecise BusFault for another exception stops at its raise")
{
  auto       m       = booted(fault_policy::stop);
  auto const stopped = run_entry(m, "masked_bus_error_twice");
  REQUIRE(stopped.reason == stop_reason::fault);
  REQUIRE(stopped.fault.has_value());
  CHECK(stopped.fault->kind == popcorn::fault_kind::bus);
  CHECK(stopped.fault->fault_address == 0x40010058);
  CHECK(stopped.fault->status == 0x00000400);
  CHECK(stopped.fault->detail == 0x40000000);
  CHECK(m.f->module->is_pending(armv8m::exc_busfault));
  CHECK_FALSE(m.f->module->is_pending(armv8m::exc_hardfault));
}

TEST_CASE("a Secure lock-until-reset sequence under TrustZone runs to its end and a release stops")
{
  for(auto const policy : {fault_policy::enter_handler, fault_policy::stop})
  {
    CAPTURE(policy);
    auto m = booted_trustzone(policy);
    REQUIRE(run_entry(m, "masked_bus_error_cleared").reason == stop_reason::user);
    auto const log = log_of(*m.f);
    REQUIRE(log.size() == 4);
    CHECK(log[0] == 0x00000400);
    CHECK((log[1] & 0x00024000) == 0x00024000);
    CHECK(log[2] == 0);
    CHECK((log[3] & 0x00024000) == 0);
    CHECK_FALSE(m.f->module->pending());
    CHECK_FALSE(deferred_of(m).has_value());
  }
  auto m = booted_trustzone(fault_policy::stop);
  REQUIRE(run_entry(m, "masked_bus_error_released").reason == stop_reason::user);
  CHECK(
      deferred_of(m)
      == armv8m::deferred_fault{
          .kind     = popcorn::fault_kind::bus,
          .reported = 0x40010058,
          .number   = armv8m::exc_busfault,
          .state    = armv8m::security::secure});
  auto const stopped = m.f->run_to_stop();
  REQUIRE(stopped.reason == stop_reason::fault);
  CHECK(stopped.fault->kind == popcorn::fault_kind::bus);
  CHECK(stopped.fault->fault_address == 0x40010058);
  CHECK(stopped.fault->status == 0x00000400);
  CHECK(stopped.where.pc == (fn("masked_release_at") & ~address{1}));
  CHECK_FALSE(deferred_of(m).has_value());
}

TEST_CASE("a Non-secure HardFault record under AIRCR.BFHFNMINS drops with its NS pended bit")
{
  auto m = booted_trustzone(fault_policy::stop);
  set_bfhfnmins(m);
  REQUIRE(run_entry(m, "masked_bus_error_forced").reason == stop_reason::user);
  auto const recorded = deferred_of(m);
  REQUIRE(recorded.has_value());
  CHECK(recorded->number == armv8m::exc_hardfault);
  CHECK(recorded->state == armv8m::security::nonsecure);
  CHECK(m.f->module->pending());
  REQUIRE(m.f->target->mem().write(0xE002'ED24, 4, 0));
  CHECK_FALSE(deferred_of(m).has_value());
  CHECK_FALSE(m.f->module->pending());
  CHECK(m.f->run_to_stop().reason == stop_reason::user);
}

TEST_CASE("a recorded BusFault that AIRCR.BFHFNMINS moves to Non-secure still stops at entry")
{
  auto m = booted_trustzone(fault_policy::stop);
  REQUIRE(run_entry(m, "masked_bus_error_released").reason == stop_reason::user);
  set_bfhfnmins(m);
  auto const stopped = m.f->run_to_stop();
  REQUIRE(stopped.reason == stop_reason::fault);
  REQUIRE(stopped.fault.has_value());
  CHECK(stopped.fault->kind == popcorn::fault_kind::bus);
  CHECK(stopped.fault->fault_address == 0x40010058);
  CHECK(stopped.where.pc == (fn("masked_release_at") & ~address{1}));
}
