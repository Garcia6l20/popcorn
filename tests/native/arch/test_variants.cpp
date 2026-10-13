#include "arch_fixture.hpp"

#include "cortex_m.hpp"
#include "state.hpp"

#include <popcorn/arch/armv8m/armv8m.hpp>
#include <popcorn/core/clock.hpp>
#include <popcorn/core/intercept.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/core/snapshot.hpp>

#include <doctest/doctest.h>

#include <reflex/serde/bson.hpp>

#include <unicorn/unicorn.h>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
namespace core   = popcorn::core;
namespace arch   = popcorn::arch;
namespace armv8m = popcorn::arch::armv8m;
using arch::call_arg;
using arch::float_abi;
using arch::value_kind;
using popcorn::address;
using popcorn::icount;
using popcorn::stop_reason;
using popcorn::vtime;
using popcorn::testing::arch_fixture;
using popcorn::testing::m33;
using popcorn::testing::m33_with;
using popcorn::testing::sym;
using popcorn::testing::words_at;

struct core_variant
{
  std::string_view cpu;
  std::string_view revision;
  bool             fpu;
  bool             fpu_double;
  std::string_view suffix;
};

constexpr std::uint32_t psp_marker = 0x5059'0000;

constexpr core_variant cm4{"CM4", "r0p1", true, false, "_m4"};
constexpr core_variant cm7{"CM7", "r1p2", true, true, "_m7"};
constexpr core_variant cm3{"CM3", "r2p1", false, false, "_m3"};

auto description(core_variant const& v) -> popcorn::part_description
{
  return m33_with([&](popcorn::part_description& d) {
    d.core.cpu            = std::string{v.cpu};
    d.core.revision       = std::string{v.revision};
    d.core.fpu            = v.fpu;
    d.core.fpu_double     = v.fpu_double;
    d.core.nvic_prio_bits = 4;
    d.core.num_irqs       = 20;
  });
}

auto name_of(std::string_view firmware, core_variant const& v) -> std::string
{
  return std::string{firmware} + std::string{v.suffix};
}

auto booted(std::string_view firmware, core_variant const& v, core::machine_options options = {})
    -> arch_fixture
{
  arch_fixture f{description(v), name_of(firmware, v), std::move(options)};
  REQUIRE(f.run_to_stop().reason == stop_reason::user);
  return f;
}

auto run_to(arch_fixture const& f, address entry) -> popcorn::stop
{
  f.set("pc", entry);
  return f.run_to_stop();
}

auto log_of(arch_fixture const& f, std::string_view firmware) -> std::vector<std::uint32_t>
{
  return words_at(*f.target, sym(firmware, "log_buf"), f.word(sym(firmware, "log_len")));
}

auto saved_special(arch_fixture const& f) -> armv8m::special_registers
{
  std::vector<std::byte> bytes;
  f.module->save(bytes);
  return reflex::serde::bson::deserializer{bytes}.load<armv8m::state>().special;
}

auto register_values(arch_fixture const& f) -> std::vector<std::uint64_t>
{
  std::vector<std::uint64_t> out;
  for(auto const& reg : f.module->registers())
  {
    out.push_back(f.reg(reg.name));
  }
  return out;
}

auto exceptions_log(core_variant const* v) -> std::vector<std::uint32_t>
{
  auto const firmware = v == nullptr ? std::string{"exceptions"} : name_of("exceptions", *v);
  auto desc = v == nullptr ? m33_with([](popcorn::part_description& d) { d.core.num_irqs = 20; })
                           : description(*v);
  arch_fixture f{std::move(desc), firmware, {}};
  REQUIRE(f.run_to_stop().reason == stop_reason::user);
  auto const hook =
      f.target->add_intercept(sym(firmware, "it_irq_label"), [](core::intercept_call const& c) {
        std::ignore = c.target().mem().write(0xE000'E200, 4, 1u << 10);
        return core::intercept_result::proceed();
      });
  REQUIRE(run_to(f, sym(firmware, "exceptions_run")).reason == stop_reason::user);
  CHECK_FALSE(f.module->pending());
  auto log = log_of(f, firmware);
  auto top = sym(firmware, "psp_stack_top");
  for(auto& word : log)
  {
    if(word == top - 0x20)
    {
      word = psp_marker;
    }
  }
  return log;
}
}

TEST_CASE("CM4 opens a Thumb engine on the M4 model without MCLASS, CPUIDs by part and revision")
{
  auto const made = armv8m::factory()(description(cm4).core);
  CHECK(
      made->engine()
      == arch::engine_spec{
          .arch = UC_ARCH_ARM, .mode = UC_MODE_THUMB, .cpu_model = UC_CPU_ARM_CORTEX_M4});
  for(auto const& [v, cpuid] : {
          std::pair{cm4, 0x410F'C241u},
          std::pair{cm7, 0x411F'C272u},
          std::pair{cm3, 0x412F'C231u}
  })
  {
    CAPTURE(v.cpu);
    auto f = booted("exceptions", v);
    CHECK(f.target->mem().read(0xE000'ED00, 4) == cpuid);
  }
}

TEST_CASE("the exception firmware logs the same sequence on M4 and M7, and without FP on M3")
{
  auto const reference = exceptions_log(nullptr);
  REQUIRE(reference.size() == 38);
  REQUIRE(std::ranges::count(reference, psp_marker) == 1);
  CHECK(exceptions_log(&cm4) == reference);
  CHECK(exceptions_log(&cm7) == reference);
  auto without_fp = reference;
  std::erase_if(without_fp, [](std::uint32_t word) { return (word & 0xF00u) == 0x700u; });
  CHECK(exceptions_log(&cm3) == without_fp);
}

TEST_CASE("M4 and M3 entry stacks the frame and return restores every register, PSP included")
{
  struct probe_case
  {
    std::string_view entry;
    std::uint32_t    lr;
    std::uint32_t    frame;
    bool             process;
    std::uint32_t    control;
    bool             fp;
  };
  std::vector<probe_case> const cases{
      {"regs_thread_msp",        0xFFFFFFF9, 0x20, false, 0, false},
      {"regs_thread_msp_fp",     0xFFFFFFE9, 0x68, false, 0, true },
      {"regs_thread_psp",        0xFFFFFFFD, 0x20, true,  0, false},
      {"regs_thread_psp_fp",     0xFFFFFFED, 0x68, true,  0, true },
      {"regs_nested",            0xFFFFFFF1, 0x20, false, 0, false},
      {"regs_nested_fp",         0xFFFFFFE1, 0x68, false, 0, true },
      {"regs_thread_psp_unpriv", 0xFFFFFFFD, 0x20, true,  1, false},
      {"regs_misaligned",        0xFFFFFFF9, 0x24, false, 0, false},
  };
  for(auto const& v : {cm4, cm3})
    for(auto const& probe : cases)
    {
      if(probe.fp and not v.fpu)
      {
        continue;
      }
      CAPTURE(v.cpu);
      CAPTURE(probe.entry);
      auto const firmware = name_of("exceptions", v);
      auto       f        = booted("exceptions", v);
      REQUIRE(run_to(f, sym(firmware, "probe_setup")).reason == stop_reason::user);
      REQUIRE(run_to(f, sym(firmware, probe.entry)).reason == stop_reason::user);
      auto const before  = register_values(f);
      auto const special = saved_special(f);
      auto const control = f.reg("control");
      auto const xpsr    = f.reg("xpsr");
      auto const used    = probe.process ? special.psp : special.msp;
      REQUIRE(f.target->mem().write(0xE000'E200, 4, 1u << 11));
      REQUIRE(f.run_to_stop().reason == stop_reason::user);
      CHECK(f.reg("lr") == probe.lr);
      CHECK(f.reg("control") == probe.control);
      auto const inside = saved_special(f);
      auto const frame  = probe.process ? inside.psp : inside.msp;
      CHECK(frame == used - probe.frame);
      auto const words = words_at(*f.target, frame, probe.frame / 4);
      CHECK(words[6] == before[14]);
      CHECK((words[7] & ~0x200u) == xpsr);
      CHECK((words[7] & (1u << 20)) == 0);
      REQUIRE(f.run_to_stop().reason == stop_reason::user);
      auto after = register_values(f);
      CHECK(after[14] == before[14] + 2);
      after[14] = before[14];
      if(not probe.fp)
      {
        after.resize(23);
      }
      auto const expected =
          probe.fp ? before : std::vector<std::uint64_t>(before.begin(), before.begin() + 23);
      CHECK(after == expected);
      CHECK(f.reg("control") == control);
      CHECK(saved_special(f) == special);
      if(probe.process)
      {
        CHECK(f.reg("psp") == special.psp);
        CHECK(f.reg("sp") == special.psp);
      }
    }
}

TEST_CASE("M4 with CCR.STKALIGN clear stacks at SP - 0x20 without the alignment bit")
{
  auto       f        = booted("exceptions", cm4);
  auto const firmware = name_of("exceptions", cm4);
  REQUIRE(run_to(f, sym(firmware, "probe_setup")).reason == stop_reason::user);
  REQUIRE(f.target->mem().read(0xE000'ED14, 4) == 0x200u);
  REQUIRE(f.target->mem().write(0xE000'ED14, 4, 0));
  REQUIRE(run_to(f, sym(firmware, "regs_misaligned")).reason == stop_reason::user);
  auto const s = saved_special(f).msp;
  REQUIRE(s % 8 == 4);
  REQUIRE(f.target->mem().write(0xE000'E200, 4, 1u << 11));
  REQUIRE(f.run_to_stop().reason == stop_reason::user);
  auto const frame = saved_special(f).msp;
  CHECK(frame == s - 0x20);
  CHECK((words_at(*f.target, frame, 8)[7] & 0x200u) == 0);
  REQUIRE(f.run_to_stop().reason == stop_reason::user);
  CHECK(saved_special(f).msp == s);
}

TEST_CASE("M4 faults enter their handlers, and a Thread return to the reset LR is a MemManage")
{
  auto const firmware = name_of("faults", cm4);
  struct row
  {
    std::string_view entry;
    std::uint32_t    code;
    std::uint32_t    cfsr;
    std::uint32_t    hfsr;
  };
  std::vector<row> const rows{
      {"udf",                0xC40, 0x00010000, 0         },
      {"udf_forced",         0xC10, 0x00010000, 0x40000000},
      {"unmapped_read",      0xC30, 0x00008200, 0         },
      {"unmapped_write",     0xC30, 0x00008200, 0         },
      {"unmapped_fetch",     0xC30, 0x00000100, 0         },
      {"flash_write",        0xC30, 0x00008200, 0         },
      {"return_to_reset_lr", 0xC20, 0x00000001, 0         },
  };
  core::machine_options enter;
  enter.faults = arch::fault_policy::enter_handler;
  for(auto const& one : rows)
  {
    CAPTURE(one.entry);
    auto f = booted("faults", cm4, enter);
    REQUIRE(run_to(f, sym(firmware, "faults_setup")).reason == stop_reason::user);
    REQUIRE(run_to(f, sym(firmware, one.entry)).reason == stop_reason::user);
    auto const log = log_of(f, firmware);
    REQUIRE(log.size() == 5);
    CHECK(log[0] == one.code);
    CHECK(log[1] == one.cfsr);
    CHECK(log[3] == one.hfsr);
  }
  auto f = booted("faults", cm4);
  REQUIRE(run_to(f, sym(firmware, "faults_setup")).reason == stop_reason::user);
  auto const stopped = run_to(f, sym(firmware, "return_to_reset_lr"));
  REQUIRE(stopped.reason == stop_reason::fault);
  CHECK(stopped.fault->kind == popcorn::fault_kind::mem_manage);
  CHECK(stopped.fault->status == 0x00000001);
}

TEST_CASE("M4 runs the SVC and PendSV logs, and SysTick ticks 1000 times a virtual second")
{
  auto const svc = name_of("svc", cm4);
  struct row
  {
    std::string_view           entry;
    std::vector<std::uint32_t> expected;
  };
  std::vector<row> const rows{
      {"svc_basic",        {0x805, 0x8FF}       },
      {"svc_handler_mode", {0x300, 0x807, 0x301}},
      {"pendsv_basic",     {0xE00, 0xE14, 0xE01}},
      {"pendsv_tail",      {0xE11, 0xE14, 0xE02}},
      {"nmi_run",          {0xE1D, 0xE04}       },
      {"stir_run",         {0xE13, 0xE05}       },
  };
  for(auto const& one : rows)
  {
    CAPTURE(one.entry);
    auto f = booted("svc", cm4);
    REQUIRE(run_to(f, sym(svc, "svc_setup")).reason == stop_reason::user);
    REQUIRE(run_to(f, sym(svc, one.entry)).reason == stop_reason::user);
    CHECK(log_of(f, svc) == one.expected);
  }
  {
    auto f = booted("svc", cm4);
    REQUIRE(run_to(f, sym(svc, "svc_setup")).reason == stop_reason::user);
    REQUIRE(run_to(f, sym(svc, "switch_run")).reason == stop_reason::user);
    CHECK(log_of(f, svc).size() == 10);
    CHECK(f.word(sym(svc, "b_fp_seen")) == 0x44000000);
  }

  auto const timers = name_of("timers", cm4);
  auto       f      = booted("timers", cm4);
  REQUIRE(run_to(f, sym(timers, "timers_setup")).reason == stop_reason::user);
  auto const stopped = run_to(f, sym(timers, "tick_phase"));
  REQUIRE(stopped.reason == stop_reason::user);
  auto const  e      = stopped.instructions.value - (3999 - f.reg("r0") + 1);
  auto const  target = e + 1000 * 4000 + 2000;
  auto const& clock  = f.target->virtual_clock();
  auto const  at     = vtime{clock.time().ps + (target - clock.now().value) * 250'000};
  auto const  result = f.run(
      {.max_instructions = icount{400'000'000},
       .wall_budget      = std::chrono::seconds{60},
       .until_time       = at});
  REQUIRE(result.stopped.has_value());
  CHECK(result.stopped->reason == stop_reason::time);
  CHECK(f.word(sym(timers, "ticks")) == 1000);
}

TEST_CASE("M4 calls follow AAPCS-VFP")
{
  auto const abi  = name_of("abi", cm4);
  auto       f    = booted("abi", cm4);
  auto const call = [&](std::string_view name, std::vector<call_arg> args, value_kind returns) {
    arch::call_frame one{
        .entry           = sym(abi, name),
        .args            = std::move(args),
        .returns         = returns,
        .abi             = float_abi::hard_double,
        .return_sentinel = 0x3000'0000};
    auto const r = f.target->call(one, icount{1'000'000});
    REQUIRE(r.stopped.reason == stop_reason::returned);
    return r.value;
  };
  auto const f32 = [](float v) {
    return call_arg{.kind = value_kind::f32, .bits = std::bit_cast<std::uint32_t>(v)};
  };
  auto const f64 = [](double v) {
    return call_arg{.kind = value_kind::f64, .bits = std::bit_cast<std::uint64_t>(v)};
  };
  auto const i32 = [](std::int32_t v) {
    return call_arg{.kind = value_kind::i32, .bits = static_cast<std::uint32_t>(v)};
  };
  auto const i64 = [](std::int64_t v) {
    return call_arg{.kind = value_kind::i64, .bits = static_cast<std::uint64_t>(v)};
  };
  CHECK(call("add", {i32(2), i32(3)}, value_kind::i32) == 5);
  CHECK(call("fadd", {f32(1.5f), f32(2.25f)}, value_kind::f32) == 0x40700000);
  CHECK(call("dmul", {f64(1.5), f64(-2.0)}, value_kind::f64) == 0xC008000000000000);
  CHECK(call("backfill", {f32(1.0f), f64(2.0), f32(3.0f)}, value_kind::f32) == 0x42F60000);
  CHECK(
      call("mixed", {i32(1), f64(2.0), i32(3), f32(4.0f), i64(5)}, value_kind::f64)
      == std::bit_cast<std::uint64_t>(54321.0));
}

TEST_CASE("the M4 MPU is PMSAv7: RBAR with VALID selects the region")
{
  auto f = booted("exceptions", cm4);
  REQUIRE(f.target->mem().write(0xE000'ED9C, 4, 0x2000'1013));
  CHECK(f.target->mem().read(0xE000'ED98, 4) == 3u);
  CHECK(f.target->mem().read(0xE000'ED9C, 4) == 0x2000'1003u);
  REQUIRE(f.target->mem().write(0xE000'EDA0, 4, 0x0306'0027));
  CHECK(f.target->mem().read(0xE000'EDA8, 4) == 0x0306'0027u);
  CHECK(f.target->mem().read(0xE000'EDC0, 4) == 0u);
}

TEST_CASE("on M33 a serialized snapshot carries MSPLIM and PSPLIM")
{
  arch_fixture f{m33(), "exceptions", {}};
  REQUIRE(f.run_to_stop().reason == stop_reason::user);
  auto special   = saved_special(f);
  special.msplim = 0x2000'0100;
  special.psplim = 0x2000'0200;
  std::vector<std::byte> bytes;
  f.module->save(bytes);
  auto state    = reflex::serde::bson::deserializer{bytes}.load<armv8m::state>();
  state.special = special;
  std::vector<std::byte> edited;
  reflex::serde::bson::serializer{edited}.dump(state);
  f.module->load(edited);
  CHECK(saved_special(f).msplim == 0x2000'0100);
  CHECK(saved_special(f).psplim == 0x2000'0200);
  auto const   bytes2 = core::encode_snapshot(f.target->snapshot().data());
  arch_fixture other{m33(), "exceptions", {}};
  REQUIRE(other.run_to_stop().reason == stop_reason::user);
  CHECK(saved_special(other).psplim == 0);
  other.target->restore(core::snapshot{core::decode_snapshot(bytes2)});
  CHECK(saved_special(other).msplim == 0x2000'0100);
  CHECK(saved_special(other).psplim == 0x2000'0200);
}

TEST_CASE("M4 refuses the ARMv8-M EXC_RETURN 0xFFFFFFBC with INVPC, and ignores msr msplim")
{
  auto const firmware = name_of("faults", cm4);
  {
    auto f = booted("faults", cm4);
    REQUIRE(run_to(f, sym(firmware, "faults_setup")).reason == stop_reason::user);
    auto const stopped = run_to(f, sym(firmware, "bad_exc_return_v8"));
    REQUIRE(stopped.reason == stop_reason::fault);
    CHECK(stopped.fault->kind == popcorn::fault_kind::usage);
    CHECK((stopped.fault->status & 0x0004'0000) != 0);
  }
  {
    auto f = booted("faults", cm4);
    REQUIRE(run_to(f, sym(firmware, "faults_setup")).reason == stop_reason::user);
    CHECK(run_to(f, sym(firmware, "stack_limit")).reason == stop_reason::user);
  }
}

TEST_CASE("ID registers, MVFR, CPACR and the M7 cache bits follow the profile")
{
  {
    auto f = booted("exceptions", cm4);
    CHECK(f.target->mem().read(0xE000'ED60, 4) == 0x0114'1110u);
    CHECK(f.target->mem().read(0xE000'ED7C, 4) == 0u);
    CHECK(f.target->mem().read(0xE000'EF40, 4) == 0x1011'0021u);
    CHECK(f.target->mem().read(0xE000'EF48, 4) == 0u);
    REQUIRE(f.target->mem().write(0xE000'ED14, 4, 0x0007'0200));
    CHECK(f.target->mem().read(0xE000'ED14, 4) == 0x0000'0200u);
  }
  {
    auto f = booted("exceptions", cm7);
    CHECK(f.target->mem().read(0xE000'ED60, 4) == 0x0110'1110u);
    CHECK(f.target->mem().read(0xE000'EF40, 4) == 0x1011'0221u);
    CHECK(f.target->mem().read(0xE000'EF44, 4) == 0x1200'0011u);
    CHECK(f.target->mem().read(0xE000'EF48, 4) == 0x0000'0040u);
    REQUIRE(f.target->mem().write(0xE000'ED14, 4, 0x0007'0200));
    CHECK(f.target->mem().read(0xE000'ED14, 4) == 0x0007'0200u);
  }
  {
    auto f = booted("exceptions", cm3);
    CHECK(f.target->mem().read(0xE000'EF40, 4) == 0u);
    REQUIRE(f.target->mem().write(0xE000'ED88, 4, 0x00F0'0000));
    CHECK(f.target->mem().read(0xE000'ED88, 4) == 0u);
  }
  {
    auto f = booted("exceptions", cm4);
    REQUIRE(f.target->mem().write(0xE000'ED88, 4, 0x00F0'0000));
    CHECK(f.target->mem().read(0xE000'ED88, 4) == 0x00F0'0000u);
  }
}
