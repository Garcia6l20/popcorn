#include "arch_fixture.hpp"

#include "cortex_m.hpp"

#include <popcorn/arch/armv8m/armv8m.hpp>
#include <popcorn/core/errors.hpp>
#include <popcorn/core/intercept.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/core/snapshot.hpp>

#include <doctest/doctest.h>

#include <reflex/serde/bson.hpp>

#include <unicorn/unicorn.h>

#include <array>
#include <cstdint>
#include <exception>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
namespace core   = popcorn::core;
namespace armv8m = popcorn::arch::armv8m;
using popcorn::address;
using popcorn::stop_reason;
using popcorn::testing::arch_fixture;
using popcorn::testing::m33;
using popcorn::testing::m33_with;
using popcorn::testing::sym;
using popcorn::testing::words_at;

constexpr address estack = 0x200C'0000;

auto fn(std::string_view name) -> address
{
  return sym("scs", name);
}

auto booted(core::machine_options options = {}) -> arch_fixture
{
  arch_fixture f{m33(), "scs", std::move(options)};
  REQUIRE(f.run_to_stop().reason == stop_reason::user);
  return f;
}

auto reread(core::snapshot const& taken) -> core::snapshot
{
  return core::snapshot{core::decode_snapshot(core::encode_snapshot(taken.data()))};
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

auto ge_of(arch_fixture const& f) -> std::uint64_t
{
  return f.reg("xpsr") & 0x000f'0000;
}

auto saved_special(arch_fixture const& f) -> armv8m::special_registers
{
  std::vector<std::byte> bytes;
  f.module->save(bytes);
  return reflex::serde::bson::deserializer{bytes}.load<armv8m::state>().special;
}

constexpr std::array<address, 14> table_registers{
    0xE000'ED00, 0xE000'ED0C, 0xE000'E400, 0xE000'E100, 0xE000'ED18, 0xE000'ED1C, 0xE000'ED20,
    0xE000'ED08, 0xE000'ED24, 0xE000'ED88, 0xE000'ED94, 0xE000'EDFC, 0xE000'E014, 0xE000'ED10};

auto table_reads(core::machine& target) -> std::vector<std::uint32_t>
{
  std::vector<std::uint32_t> out;
  for(auto const at : table_registers)
  {
    auto const word = target.mem().read(at, 4);
    REQUIRE(word.has_value());
    out.push_back(*word);
  }
  return out;
}

auto run_scs() -> std::pair<std::vector<std::uint32_t>, std::vector<std::uint64_t>>
{
  auto f = booted();
  f.set("pc", fn("scs_run"));
  REQUIRE(f.run_to_stop().reason == stop_reason::user);
  auto const count = f.word(sym("scs", "result_count"));
  return {words_at(*f.target, sym("scs", "results"), count), register_values(f)};
}
}

TEST_CASE("the factory implements CM33 only and opens an M33 engine")
{
  auto core = m33().core;
  CHECK(armv8m::implements(core));
  core.cpu = "CM4";
  CHECK_FALSE(armv8m::implements(core));
  core.cpu = "XYZ";
  CHECK_FALSE(armv8m::implements(core));
  core.cpu = "CM4";
  try
  {
    std::ignore = armv8m::factory()(core);
    FAIL("CM4 accepted");
  }
  catch(popcorn::config_error const& refused)
  {
    CHECK(refused.key() == "core.cpu");
  }
  core          = m33().core;
  core.revision = "rXp4";
  CHECK_THROWS_AS(std::ignore = armv8m::factory()(core), popcorn::config_error);
  auto const made = armv8m::factory()(m33().core);
  CHECK(
      made->engine()
      == popcorn::arch::engine_spec{
          .arch      = UC_ARCH_ARM,
          .mode      = UC_MODE_THUMB | UC_MODE_MCLASS,
          .cpu_model = UC_CPU_ARM_CORTEX_M33});
}

TEST_CASE("a peripheral in the core's system space is refused")
{
  auto desc = m33_with([](popcorn::mcu_description& d) {
    auto dcb = d.peripherals.front();
    dcb.name = "DCB";
    dcb.base = 0xE000'EE08;
    dcb.size = 5;
    dcb.registers.clear();
    dcb.cross_rules.clear();
    dcb.irqs.clear();
    dcb.alias_of.reset();
    d.peripherals.push_back(dcb);
  });
  try
  {
    arch_fixture f{std::move(desc), "scs", {}};
    FAIL("DCB accepted");
  }
  catch(popcorn::config_error const& refused)
  {
    CHECK(refused.key() == "peripherals.DCB");
  }
}

TEST_CASE("reset loads the vector table and zeroes the core")
{
  arch_fixture f{m33(), "scs", {}};
  CHECK(f.reg("pc") == fn("Reset_Handler"));
  CHECK(f.reg("msp") == estack);
  CHECK(f.reg("sp") == estack);
  CHECK(f.reg("lr") == 0xffff'ffff);
  CHECK(f.reg("xpsr") == 0x0100'0000);
  CHECK(f.reg("control") == 0);
  CHECK(f.reg("primask") == 0);
  CHECK(f.reg("basepri") == 0);
  CHECK(f.reg("faultmask") == 0);
  CHECK(f.reg("r0") == 0);
  CHECK(f.reg("s31") == 0);
}

TEST_CASE("registers are listed in snapshot order and round trip")
{
  arch_fixture                  f{m33(), "scs", {}};
  std::vector<std::string_view> names;
  for(auto const& reg : f.module->registers())
  {
    names.push_back(reg.name);
  }
  std::vector<std::string_view> expected{
      "r0",      "r1",      "r2",        "r3",      "r4",  "r5",  "r6", "r7",
      "r8",      "r9",      "r10",       "r11",     "r12", "lr",  "pc", "xpsr",
      "primask", "basepri", "faultmask", "control", "msp", "psp", "sp"};
  for(int index = 0; index < 32; ++index)
  {
    static std::array<std::string, 32> const s_names = [] {
      std::array<std::string, 32> out;
      for(int at = 0; at < 32; ++at)
      {
        out[static_cast<std::size_t>(at)] = "s" + std::to_string(at);
      }
      return out;
    }();
    expected.push_back(s_names[static_cast<std::size_t>(index)]);
  }
  expected.push_back("fpscr");
  CHECK(names == expected);
  CHECK(names.size() == 56);

  std::vector<std::uint64_t> written;
  for(auto const& [index, reg] : std::views::enumerate(f.module->registers()))
  {
    auto const    name  = reg.name;
    std::uint64_t value = 0x1111'0000u + static_cast<std::uint64_t>(index) * 0x0101u;
    if(name == "pc")
    {
      value = 0x0800'0100;
    }
    else if(name == "xpsr")
    {
      value = 0xf100'0000;
    }
    else if(name == "primask" or name == "faultmask")
    {
      value = 1;
    }
    else if(name == "basepri")
    {
      value = 0x40;
    }
    else if(name == "control")
    {
      value = 0;
    }
    else if(name == "msp" or name == "sp")
    {
      value = 0x2000'1000;
    }
    else if(name == "psp")
    {
      value = 0x2000'2000;
    }
    else if(name == "fpscr")
    {
      value = 0x0000'0000;
    }
    f.set(name, value);
    written.push_back(value);
  }
  CHECK(register_values(f) == written);
  f.set("control", 2);
  CHECK(f.reg("sp") == 0x2000'2000);
}

TEST_CASE("special registers survive a register-list restore into and out of unprivileged state")
{
  SUBCASE("snapshot taken unprivileged on PSP")
  {
    auto f = booted();
    f.set("pc", fn("unpriv_psp"));
    REQUIRE(f.run_to_stop().reason == stop_reason::user);
    REQUIRE(f.reg("control") == 3);
    auto const taken    = reread(f.target->snapshot());
    auto const expected = register_values(f);
    auto const ge       = ge_of(f);
    REQUIRE(ge == 0x000a'0000);
    auto const special = saved_special(f);
    CHECK(special.basepri == 0x40);
    CHECK(special.psp == 0x2007'0000);
    CHECK(special.msp == estack);
    f.target->reset();
    f.target->restore(taken);
    CHECK(register_values(f) == expected);
    CHECK(f.reg("control") == 3);
    CHECK(ge_of(f) == ge);
    CHECK(saved_special(f) == special);
    auto other = booted();
    other.target->restore(taken);
    CHECK(register_values(other) == expected);
    CHECK(other.reg("control") == 3);
    CHECK(ge_of(other) == ge);
    CHECK(saved_special(other) == special);
  }
  SUBCASE("snapshot taken privileged, restored while unprivileged")
  {
    auto       f        = booted();
    auto const taken    = reread(f.target->snapshot());
    auto const expected = register_values(f);
    f.set("pc", fn("unpriv_psp"));
    REQUIRE(f.run_to_stop().reason == stop_reason::user);
    REQUIRE(f.reg("control") == 3);
    f.target->restore(taken);
    CHECK(f.reg("control") == 0);
    CHECK(f.reg("msp") == expected[20]);
    CHECK(f.reg("psp") == expected[21]);
    CHECK(f.reg("primask") == expected[16]);
    CHECK(f.reg("basepri") == 0);
    CHECK(register_values(f) == expected);
  }
}

TEST_CASE("reset restores the engine's pristine context, MSPLIM included")
{
  auto f = booted();
  f.set("pc", fn("msplim_set"));
  REQUIRE(f.run_to_stop().reason == stop_reason::user);
  f.target->reset();
  REQUIRE(f.run_to_stop().reason == stop_reason::user);
  f.set("pc", fn("deep_stack"));
  auto const stopped = f.run_to_stop();
  CHECK(stopped.reason == stop_reason::user);
  CHECK(stopped.where.pc == fn("deep_stack") + 12);
}

TEST_CASE("the system space reads back what the architecture defines")
{
  auto const [results, registers] = run_scs();
  std::vector<std::uint32_t> const expected{
      0x410F'D214, 0x0000'0000, 0xFA05'0000, 0xFA05'0500, 0xFA05'0500, 0x0020'4060, 0x0000'0000,
      0x0000'00FF, 0x0000'0001, 0x0000'0000, 0x00E0'E0E0, 0xE000'0000, 0xE0E0'00E0, 0x2000'0000,
      0x0000'0201, 0x0007'0000, 0x00F0'0000, 0xC000'0000, 0x0000'0800, 0x2000'1003, 0x0000'0005,
      0x0100'0000, 0x4000'0000, 0x0000'0001, 0x00FF'FFFF, 0x0000'0016, 0x0000'0000};
  CHECK(results == expected);
}

TEST_CASE("AIRCR SYSRESETREQ requests a software reset only with the key")
{
  SUBCASE("stop policy")
  {
    auto f = booted();
    f.set("pc", fn("reset_req"));
    auto const stopped = f.run_to_stop();
    CHECK(stopped.reason == stop_reason::reset_request);
    CHECK(stopped.detail == "software");
  }
  SUBCASE("reset and continue")
  {
    core::machine_options options;
    options.on_reset_request = core::reset_request_policy::reset_and_continue;
    arch_fixture f{m33(), "scs", options};
    REQUIRE(f.target->mem().write(0x2008'0000, 4, 0));
    REQUIRE(f.run_to_stop().reason == stop_reason::user);
    f.set("pc", fn("reset_req"));
    REQUIRE(f.run_to_stop().reason == stop_reason::user);
    CHECK(f.word(0x2008'0000) == 2);
  }
  SUBCASE("wrong key")
  {
    auto f = booted();
    f.set("pc", fn("reset_req_wrong_key"));
    CHECK(f.run_to_stop().reason == stop_reason::user);
  }
}

TEST_CASE("the debugger reads the system space between slices and from a handler")
{
  auto f = booted();
  CHECK(f.target->mem().read(0xE000'ED00, 4) == 0x410F'D214u);
  std::optional<std::uint32_t> seen;
  auto const hook = f.target->add_intercept(fn("scs_run"), [&](core::intercept_call const& call) {
    auto const cpuid = call.target().mem().read(0xE000'ED00, 4);
    seen             = cpuid ? std::optional{*cpuid} : std::nullopt;
    return core::intercept_result::proceed();
  });
  f.set("pc", fn("scs_run"));
  REQUIRE(f.run_to_stop().reason == stop_reason::user);
  CHECK(seen == 0x410F'D214u);
}

TEST_CASE("snapshots carry the system space, into this machine and another")
{
  auto f = booted();
  f.set("pc", fn("scs_run"));
  REQUIRE(f.run_to_stop().reason == stop_reason::user);
  auto const before = table_reads(*f.target);
  auto const taken  = f.target->snapshot();
  f.target->reset();
  CHECK(table_reads(*f.target) != before);
  f.target->restore(taken);
  CHECK(table_reads(*f.target) == before);
  auto other = booted();
  other.target->restore(reread(taken));
  CHECK(table_reads(*other.target) == before);
}

TEST_CASE("the module state round trips through its saved bytes")
{
  auto f = booted();
  f.set("pc", fn("scs_run"));
  REQUIRE(f.run_to_stop().reason == stop_reason::user);
  REQUIRE(f.target->mem().write(0xE000'1020, 4, 0x2000'1234));
  REQUIRE(f.target->mem().write(0xE000'1038, 4, 0x0000'0005));
  std::vector<std::byte> first;
  f.module->save(first);
  f.module->load(first);
  std::vector<std::byte> second;
  f.module->save(second);
  CHECK(first == second);
  CHECK(f.target->mem().read(0xE000'1020, 4) == 0x2000'1234u);
  CHECK(f.target->mem().read(0xE000'1038, 4) == 0x0000'0005u);
  CHECK(
      reflex::serde::bson::deserializer{second}.load<armv8m::state>().scs.dwt_comparators[3] == 5);
}

TEST_CASE("a failure kept by a callback leaves the next slice with it, once")
{
  auto f = booted();
  f.module->keep_failure(std::make_exception_ptr(popcorn::emulation_error("x")));
  f.set("pc", fn("scs_run"));
  CHECK_THROWS_AS(std::ignore = f.run(), popcorn::emulation_error);
  CHECK(f.run_to_stop().reason == stop_reason::user);
}

TEST_CASE("the system space run is deterministic")
{
  CHECK(run_scs() == run_scs());
}

TEST_CASE("the register view reads and writes special registers while unprivileged")
{
  auto f = booted();
  f.set("pc", fn("unpriv_psp"));
  REQUIRE(f.run_to_stop().reason == stop_reason::user);
  REQUIRE(f.reg("control") == 3);
  CHECK(f.reg("basepri") == 0x40);
  CHECK(f.reg("psp") == 0x2007'0000);
  CHECK(f.reg("msp") == estack);
  CHECK(f.reg("sp") == 0x2007'0000);
  f.set("primask", 1);
  f.set("basepri", 0x60);
  f.set("faultmask", 1);
  f.set("msp", 0x2000'1000);
  f.set("psp", 0x2000'2000);
  CHECK(f.reg("primask") == 1);
  CHECK(f.reg("basepri") == 0x60);
  CHECK(f.reg("faultmask") == 1);
  CHECK(f.reg("msp") == 0x2000'1000);
  CHECK(f.reg("psp") == 0x2000'2000);
  CHECK(f.reg("sp") == 0x2000'2000);
  f.set("control", 0);
  CHECK(f.reg("control") == 0);
  CHECK(f.reg("sp") == 0x2000'1000);
  f.set("control", 3);
  CHECK(f.reg("control") == 3);
  CHECK(f.reg("sp") == 0x2000'2000);
  CHECK(f.reg("basepri") == 0x60);
}
