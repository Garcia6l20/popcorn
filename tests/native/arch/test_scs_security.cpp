#include "arch_fixture.hpp"

#include "../../../native/src/core/machine_impl.hpp"
#include "cortex_m.hpp"
#include "state.hpp"

#include <popcorn/core/errors.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/core/snapshot.hpp>

#include <doctest/doctest.h>

#include <reflex/serde/bson.hpp>

#include <unicorn/unicorn.h>

#include <array>
#include <cstdint>
#include <initializer_list>
#include <utility>
#include <vector>

namespace
{
namespace core   = popcorn::core;
namespace armv8m = popcorn::arch::armv8m;
using popcorn::address;
using popcorn::fault_kind;
using popcorn::icount;
using popcorn::stop_reason;
using popcorn::testing::arch_fixture;
using popcorn::testing::m33_trustzone;
using popcorn::testing::trustzone_options;

constexpr address code_at     = 0x2008'0000;
constexpr address secure_code = 0x2008'2000;
constexpr address frame_at    = 0x2008'3000;
constexpr address scs_ns      = 0x0002'0000;

constexpr address vtor   = 0xE000'ED08;
constexpr address aircr  = 0xE000'ED0C;
constexpr address icsr   = 0xE000'ED04;
constexpr address shpr1  = 0xE000'ED18;
constexpr address shcsr  = 0xE000'ED24;
constexpr address cpacr  = 0xE000'ED88;
constexpr address fpccr  = 0xE000'EF34;
constexpr address nsacr  = 0xE000'ED8C;
constexpr address iser0  = 0xE000'E100;
constexpr address ipr0   = 0xE000'E400;
constexpr address itns0  = 0xE000'E380;
constexpr address syst   = 0xE000'E010;
constexpr address mpu    = 0xE000'ED90;
constexpr address sau    = 0xE000'EDD0;
constexpr address sau_rn = 0xE000'EDD8;
constexpr address sau_rb = 0xE000'EDDC;
constexpr address sau_rl = 0xE000'EDE0;

constexpr std::uint32_t sau_enable = 1u << 0;
constexpr std::uint32_t sau_allns  = 1u << 1;
constexpr std::uint32_t tt_srvalid = 1u << 17;
constexpr std::uint32_t tt_mrvalid = 1u << 16;
constexpr std::uint32_t fill       = 0xA5A5'A5A5;
constexpr std::uint32_t one_float  = 0x3F80'0000;

constexpr std::uint32_t fpccr_s         = 1u << 2;
constexpr std::uint32_t fpccr_lspact    = 1u << 0;
constexpr std::uint32_t fpccr_ts        = 1u << 26;
constexpr std::uint32_t fpccr_clronrets = 1u << 27;
constexpr std::uint32_t fpccr_clronret  = 1u << 28;
constexpr std::uint32_t fpccr_lspen     = 1u << 30;
constexpr std::uint32_t fpccr_aspen     = 1u << 31;
constexpr std::uint32_t control_fpca    = 1u << 2;
constexpr std::uint32_t control_sfpa    = 1u << 3;

auto booted(bool trustzone = true, std::uint8_t secure_mpu = 8) -> arch_fixture
{
  arch_fixture f{
      m33_trustzone(secure_mpu), "scs", trustzone ? trustzone_options() : core::machine_options{}};
  REQUIRE(f.run_to_stop().reason == stop_reason::user);
  return f;
}

auto engine_of(arch_fixture const& f) -> uc_struct*
{
  return core::machine_internals::engine(*f.target);
}

auto engine_read(arch_fixture const& f, int id) -> std::uint32_t
{
  std::uint32_t value = 0;
  REQUIRE(uc_reg_read(engine_of(f), id, &value) == UC_ERR_OK);
  return value;
}

void engine_write(arch_fixture const& f, int id, std::uint32_t value)
{
  REQUIRE(uc_reg_write(engine_of(f), id, &value) == UC_ERR_OK);
}

void put(arch_fixture const& f, address at, std::initializer_list<std::uint16_t> halves)
{
  for(auto const half : halves)
  {
    REQUIRE(f.target->mem().write(at, 2, half));
    at += 2;
  }
}

void poke(arch_fixture const& f, address at, std::uint32_t value)
{
  REQUIRE(f.target->mem().write(at, 4, value));
}

auto run_at(arch_fixture const& f, address entry, std::uint64_t bound) -> popcorn::stop
{
  f.set("pc", entry | 1u);
  return f.run_to_stop({.max_instructions = icount{bound}});
}

auto nonsecure_access(arch_fixture const& f, address at, std::uint16_t insn, std::uint32_t r1 = 0)
    -> std::uint32_t
{
  poke(f, sau, sau_allns);
  engine_write(f, UC_ARM_REG_SECURE, 0);
  put(f, code_at, {insn, 0xbe01});
  f.set("r0", at);
  f.set("r1", r1);
  REQUIRE(run_at(f, code_at, 2).reason == stop_reason::user);
  engine_write(f, UC_ARM_REG_SECURE, 1);
  poke(f, sau, 0);
  return static_cast<std::uint32_t>(f.reg("r1"));
}

auto nonsecure_read(arch_fixture const& f, address at) -> std::uint32_t
{
  return nonsecure_access(f, at, 0x6801);
}

void nonsecure_write(arch_fixture const& f, address at, std::uint32_t value)
{
  std::ignore = nonsecure_access(f, at, 0x6001, value);
}

void fill_frame(arch_fixture const& f)
{
  for(address offset = 0; offset < 0x100; offset += 4)
  {
    poke(f, frame_at + offset, fill);
  }
}

auto run_vlstm(arch_fixture const& f) -> popcorn::stop
{
  fill_frame(f);
  put(f, secure_code, {0xee00, 0x2a10, 0xec20, 0x0a00, 0xbe01});
  f.set("r0", frame_at);
  f.set("r2", one_float);
  return run_at(f, secure_code, 3);
}

void step(arch_fixture const& f, std::uint64_t count)
{
  auto const result = f.run({.exact_instructions = icount{count}});
  REQUIRE(result.stopped.has_value());
  REQUIRE(result.stopped->reason == stop_reason::budget);
}

void seed(arch_fixture const& f, auto&& edit)
{
  std::vector<std::byte> bytes;
  f.module->save(bytes);
  auto saved = reflex::serde::bson::deserializer{bytes}.load<armv8m::state>();
  edit(saved);
  std::vector<std::byte> edited;
  reflex::serde::bson::serializer{edited}.dump(saved);
  f.module->load(edited);
}

auto reread(core::snapshot const& taken) -> core::snapshot
{
  return core::snapshot{core::decode_snapshot(core::encode_snapshot(taken.data()))};
}
}

TEST_CASE("VTOR_S and VTOR_NS are distinct and SCS_NS is RAZ/WI to Non-secure, D1.2 VTOR and D1.1")
{
  auto f = booted();
  poke(f, vtor, 0x0800'0000);
  poke(f, vtor + scs_ns, 0x0801'0000);
  CHECK(f.word(vtor) == 0x0800'0000);
  CHECK(f.word(vtor + scs_ns) == 0x0801'0000);
  CHECK(nonsecure_read(f, vtor) == 0x0801'0000);
  CHECK(nonsecure_read(f, vtor + scs_ns) == 0);
  nonsecure_write(f, vtor + scs_ns, 0x0802'0000);
  CHECK(f.word(vtor + scs_ns) == 0x0801'0000);
  nonsecure_write(f, vtor, 0x0803'0000);
  CHECK(f.word(vtor + scs_ns) == 0x0803'0000);
  CHECK(f.word(vtor) == 0x0800'0000);
}

TEST_CASE("AIRCR keeps BFHFNMINS, PRIS and SYSRESETREQS from Secure only, D1.2.3")
{
  auto f = booted();
  poke(f, aircr, 0x05FA'4300);
  CHECK(f.word(aircr) == 0xFA05'4300);
  nonsecure_write(f, aircr, 0x05FA'2508);
  CHECK(nonsecure_read(f, aircr) == 0xFA05'0500);
  CHECK(f.word(aircr) == 0xFA05'4300);
  poke(f, aircr, 0x05FA'2300);
  CHECK(f.word(aircr) == 0xFA05'2300);
  CHECK(nonsecure_read(f, aircr) == 0xFA05'2500);
}

TEST_CASE("a Secure AIRCR write of BFHFNMINS reads back set, and under TZEN=0 is dropped")
{
  auto secure = booted();
  poke(secure, aircr, 0x05FA'2000);
  CHECK(secure.word(aircr) == 0xFA05'2000);
  auto plain = booted(false);
  poke(plain, aircr, 0x05FA'2000);
  CHECK(plain.word(aircr) == 0xFA05'0000);
}

TEST_CASE("NVIC_ITNS hides Secure IRQs from Non-secure ISER and IPR, D1.2 NVIC_ITNSn and B3.11")
{
  auto f = booted();
  poke(f, itns0, 1u << 5);
  CHECK(f.word(itns0) == 1u << 5);
  CHECK(f.word(itns0 + scs_ns) == 0);
  nonsecure_write(f, iser0, (1u << 5) | (1u << 6));
  CHECK(f.word(iser0) == 1u << 5);
  poke(f, iser0, 1u << 6);
  CHECK(nonsecure_read(f, iser0) == 1u << 5);
  nonsecure_write(f, ipr0 + 4, 0x0000'e0a0);
  CHECK(f.word(ipr0 + 4) == 0x0000'e000);
  nonsecure_write(f, itns0, 0xff);
  CHECK(f.word(itns0) == 1u << 5);
}

TEST_CASE("SysTick_S and SysTick_NS count apart, each with its own event, B3.11 RJQMQ")
{
  auto f = booted();
  engine_write(f, UC_ARM_REG_PRIMASK_S, 1);
  put(f, code_at, {0xe7fe});
  poke(f, syst + 4, 999);
  poke(f, syst + 8, 0);
  poke(f, syst, 0x5);
  poke(f, syst + scs_ns + 4, 49);
  poke(f, syst + scs_ns + 8, 0);
  poke(f, syst + scs_ns, 0x7);
  f.set("pc", code_at | 1u);
  step(f, 120);
  CHECK((f.word(icsr + scs_ns) >> 26 & 1u) == 1);
  CHECK((f.word(icsr) >> 26 & 1u) == 0);
  CHECK((f.word(syst + scs_ns) >> 16 & 1u) == 1);
  CHECK((f.word(syst) >> 16 & 1u) == 0);
  step(f, 1000);
  CHECK((f.word(syst) >> 16 & 1u) == 1);
  CHECK((f.word(icsr) >> 26 & 1u) == 0);
}

TEST_CASE("the SAU registers are Secure only and reach the engine, TT reports the region, D1.2.225")
{
  auto f = booted();
  poke(f, sau_rn, 3);
  poke(f, sau_rb, code_at | 0x1f);
  poke(f, sau_rl, (code_at + 0x3e0) | 1u);
  poke(f, sau, sau_enable);
  CHECK(f.word(sau_rb) == code_at);
  CHECK(f.word(sau_rl) == ((code_at + 0x3e0) | 1u));
  CHECK(f.word(sau + 4) == 8);
  CHECK(f.word(sau) == sau_enable);
  engine_write(f, UC_ARM_REG_SECURE, 0);
  put(f, code_at, {0x6801, 0x6002, 0xbe01});
  f.set("r0", sau);
  f.set("r2", 0);
  REQUIRE(run_at(f, code_at, 3).reason == stop_reason::user);
  CHECK(f.reg("r1") == 0);
  engine_write(f, UC_ARM_REG_SECURE, 1);
  CHECK(f.word(sau) == sau_enable);
  put(f, secure_code, {0xe840, 0xf100, 0xbe01});
  f.set("r0", code_at + 0x100);
  REQUIRE(run_at(f, secure_code, 2).reason == stop_reason::user);
  auto const tt = static_cast<std::uint32_t>(f.reg("r1"));
  CHECK((tt & tt_srvalid) != 0);
  CHECK((tt >> 8 & 0xffu) == 3);
}

TEST_CASE("TT reports MRVALID 0 for a Secure MPU region, the MPU being storage only, FR-CORE-9")
{
  auto f = booted();
  poke(f, mpu + 8, 0);
  poke(f, mpu + 0xc, code_at);
  poke(f, mpu + 0x10, (code_at + 0x3e0) | 1u);
  poke(f, mpu + 4, 1);
  CHECK(f.word(mpu + 4) == 1);
  put(f, secure_code, {0xe840, 0xf100, 0xbe01});
  f.set("r0", code_at + 0x100);
  REQUIRE(run_at(f, secure_code, 2).reason == stop_reason::user);
  CHECK((f.reg("r1") & tt_mrvalid) == 0);
}

TEST_CASE("a SAU write that denies the loop's page ends the run, the next fetch faults, B3.15")
{
  auto f = booted();
  poke(f, sau_rn, 0);
  poke(f, sau_rb, code_at);
  poke(f, sau_rl, code_at + 0x3e0);
  poke(f, sau, sau_enable);
  put(f, code_at,
      {0x3301, 0x42ab, 0xbf08, 0x600a, 0xf3bf, 0x8f4f, 0xf3bf, 0x8f6f, 0x42bb, 0xd1f5, 0xbe02});
  f.set("r1", sau_rl);
  f.set("r2", (code_at + 0x3e0) | 1u);
  f.set("r3", 0);
  f.set("r5", 10);
  f.set("r7", 100'000);
  auto const stopped = run_at(f, code_at, 1'000'000);
  REQUIRE(stopped.reason == stop_reason::fault);
  CHECK(f.reg("r3") == 10);
  CHECK(stopped.where.pc == code_at + 8);
}

TEST_CASE(
    "a jump into a page a SAU write denies faults, the engine looking up every jump across pages")
{
  auto              f     = booted();
  constexpr address chain = code_at + 0x1000;
  constexpr address far   = code_at + 0x1800;
  poke(f, sau_rn, 0);
  poke(f, sau_rb, far);
  poke(f, sau_rl, far + 0x3e0);
  poke(f, sau, sau_enable);
  put(f, chain,
      {0x42ab, 0xbf08, 0x600a, 0xf3bf, 0x8f4f, 0xf3bf, 0x8f6f, 0xe7ff, 0x3301, 0x42bb, 0xd001,
       0xf000, 0xbbf3, 0xbe02});
  put(f, far, {0xf7ff, 0xbbfe});
  f.set("r1", sau_rl);
  f.set("r2", (far + 0x3e0) | 1u);
  f.set("r3", 0);
  f.set("r5", 10);
  f.set("r7", 100'000);
  auto const stopped = run_at(f, chain, 1'000'000);
  REQUIRE(stopped.reason == stop_reason::fault);
  CHECK(f.reg("r3") == 11);
  CHECK(stopped.where.pc == far);
}

TEST_CASE("a SAU write of the stored value keeps the run and the translated code")
{
  auto       f       = booted();
  auto const plain   = code_at + 0x1900;
  auto const runs_of = [&](address target, std::uint32_t value) {
    put(f, plain, {0x600a, 0x3301, 0x42bb, 0xd1fb, 0xbe02});
    f.set("r1", target);
    f.set("r2", value);
    f.set("r3", 0);
    f.set("r7", 1000);
    auto const before = core::machine_internals::counters(*f.target).engine_runs;
    REQUIRE(run_at(f, plain, 1'000'000).reason == stop_reason::user);
    return core::machine_internals::counters(*f.target).engine_runs - before;
  };
  poke(f, sau_rn, 0);
  poke(f, sau_rl, 0x2009'0001);
  auto const control = runs_of(frame_at, 0);
  CHECK(runs_of(sau_rl, 0x2009'0001) == control);
}

TEST_CASE("BFHFNMINS 0 makes the BusFault and NMI bits RAZ/WI to Non-secure, B3.11 RHXRW")
{
  auto f = booted();
  poke(f, shpr1, 0x0000'4000);
  poke(f, shcsr, 1u << 17);
  CHECK(f.word(shpr1 + scs_ns) == 0);
  CHECK((f.word(shcsr + scs_ns) >> 17 & 1u) == 0);
  poke(f, shpr1 + scs_ns, 0x0000'2000);
  poke(f, shcsr + scs_ns, 0);
  poke(f, icsr + scs_ns, 1u << 31);
  CHECK(f.word(shpr1) == 0x0000'4000);
  CHECK((f.word(shcsr) >> 17 & 1u) == 1);
  CHECK((f.word(icsr) >> 31) == 0);
  poke(f, aircr, 0x05FA'2000);
  CHECK(f.word(shpr1 + scs_ns) == 0x0000'4000);
  CHECK((f.word(shcsr + scs_ns) >> 17 & 1u) == 1);
  poke(f, icsr, 1u << 31);
  CHECK((f.word(icsr + scs_ns) >> 31) == 1);
}

TEST_CASE("BFHFNMINS gates the Non-secure view of CFSR BFSR, its byte clear, HFSR and BFAR, D1.2")
{
  auto f = booted();
  seed(f, [](armv8m::state& s) {
    s.scs.cfsr = 0x0000'8200;
    s.scs.hfsr = 0x4000'0000;
    s.scs.bfar = 0x2008'4000;
  });
  CHECK(f.word(0xE000'ED28 + scs_ns) == 0);
  CHECK(f.word(0xE000'ED2C + scs_ns) == 0);
  CHECK(f.word(0xE000'ED38 + scs_ns) == 0);
  REQUIRE(f.target->mem().write(0xE000'ED29 + scs_ns, 1, 0x02));
  poke(f, 0xE000'ED38 + scs_ns, 0);
  CHECK(f.word(0xE000'ED28) == 0x0000'8200);
  CHECK(f.word(0xE000'ED38) == 0x2008'4000);
  poke(f, aircr, 0x05FA'2000);
  CHECK(f.word(0xE000'ED28 + scs_ns) == 0x0000'8200);
  CHECK(f.word(0xE000'ED2C + scs_ns) == 0x4000'0000);
  CHECK(f.word(0xE000'ED38 + scs_ns) == 0x2008'4000);
  REQUIRE(f.target->mem().write(0xE000'ED29 + scs_ns, 1, 0x02));
  CHECK(f.word(0xE000'ED28) == 0x0000'8000);
}

TEST_CASE("CCR.BFHFNMIGN is read-only to Non-secure while BFHFNMINS is 0, D1.2 CCR")
{
  auto f = booted();
  poke(f, 0xE000'ED14 + scs_ns, 1u << 8);
  CHECK((f.word(0xE000'ED14) >> 8 & 1u) == 0);
  poke(f, 0xE000'ED14, 1u << 8);
  CHECK((f.word(0xE000'ED14 + scs_ns) >> 8 & 1u) == 1);
  poke(f, aircr, 0x05FA'2000);
  poke(f, 0xE000'ED14 + scs_ns, 0);
  CHECK((f.word(0xE000'ED14) >> 8 & 1u) == 0);
}

TEST_CASE("BFHFNMINS gates the Non-secure SHCSR BusFault, NMI and HardFault bits, D1.2 SHCSR")
{
  auto f = booted();
  seed(f, [](armv8m::state& s) {
    armv8m::put_bit(s.exceptions.active, armv8m::exc_busfault, true);
    armv8m::put_bit(s.exceptions.active, armv8m::exc_nmi, true);
    armv8m::put_bit(s.exceptions.pending, armv8m::exc_busfault, true);
    s.exceptions.nonsecure_pending = 1u << armv8m::exc_hardfault;
  });
  constexpr std::uint32_t gated = (1u << 1) | (1u << 5) | (1u << 14) | (1u << 21);
  CHECK((f.word(shcsr + scs_ns) & gated) == 0);
  poke(f, shcsr + scs_ns, 0);
  CHECK((f.word(shcsr) >> 14 & 1u) == 1);
  poke(f, aircr, 0x05FA'2000);
  CHECK((f.word(shcsr + scs_ns) & gated) == gated);
  poke(f, shcsr + scs_ns, 0);
  CHECK((f.word(shcsr + scs_ns) & ((1u << 14) | (1u << 21))) == 0);
}

TEST_CASE("DEMCR.SDME hides the DebugMonitor priority and MONITORACT from Non-secure, B3.11")
{
  auto f = booted();
  poke(f, 0xE000'ED20, 0x0000'0040);
  seed(f, [](armv8m::state& s) {
    armv8m::put_bit(s.exceptions.active, armv8m::exc_debugmon, true);
    s.scs.demcr |= 1u << 20;
  });
  CHECK((f.word(0xE000'ED20 + scs_ns) & 0xffu) == 0);
  CHECK((f.word(shcsr + scs_ns) >> 8 & 1u) == 0);
  poke(f, 0xE000'ED20 + scs_ns, 0x0000'0020);
  CHECK((f.word(0xE000'ED20) & 0xffu) == 0x40);
  poke(f, 0xE000'EDFC, 1u << 20);
  CHECK((f.word(0xE000'EDFC) >> 20 & 1u) == 1);
  seed(f, [](armv8m::state& s) { s.scs.demcr &= ~(1u << 20); });
  CHECK((f.word(0xE000'ED20 + scs_ns) & 0xffu) == 0x40);
  CHECK((f.word(shcsr + scs_ns) >> 8 & 1u) == 1);
  poke(f, 0xE000'EDFC, 1u << 20);
  CHECK((f.word(0xE000'EDFC) >> 20 & 1u) == 0);
}

TEST_CASE("FPCCR HFRDY, BFRDY and MONRDY follow BFHFNMINS and SDME for Non-secure, D1.2.100")
{
  constexpr std::uint32_t hfrdy  = 1u << 4;
  constexpr std::uint32_t bfrdy  = 1u << 6;
  constexpr std::uint32_t monrdy = 1u << 8;
  auto                    f      = booted();
  poke(f, fpccr + scs_ns, f.word(fpccr + scs_ns) | hfrdy | bfrdy | monrdy);
  CHECK((f.word(fpccr) & (hfrdy | bfrdy)) == 0);
  CHECK((f.word(fpccr) & monrdy) == monrdy);
  poke(f, fpccr, f.word(fpccr) | hfrdy | bfrdy);
  CHECK((f.word(fpccr + scs_ns) & (hfrdy | bfrdy)) == 0);
  seed(f, [](armv8m::state& s) { s.scs.demcr |= 1u << 20; });
  CHECK((f.word(fpccr + scs_ns) & monrdy) == 0);
  poke(f, fpccr + scs_ns, f.word(fpccr + scs_ns) & ~monrdy);
  CHECK((f.word(fpccr) & monrdy) == monrdy);
  poke(f, aircr, 0x05FA'2000);
  CHECK((f.word(fpccr + scs_ns) & (hfrdy | bfrdy)) == (hfrdy | bfrdy));
  poke(f, fpccr + scs_ns, f.word(fpccr + scs_ns) & ~(hfrdy | bfrdy));
  CHECK((f.word(fpccr) & (hfrdy | bfrdy)) == 0);
}

TEST_CASE("a Non-secure STIR write pends only an IRQ that ITNS targets Non-secure, D1.2 STIR")
{
  auto f = booted();
  poke(f, itns0, 1u << 5);
  poke(f, 0xE000'EF00 + scs_ns, 6);
  CHECK(f.word(0xE000'E200) == 0);
  poke(f, 0xE000'EF00 + scs_ns, 5);
  CHECK(f.word(0xE000'E200) == 1u << 5);
}

TEST_CASE("a Secure store to SCS_NS lands in VTOR_NS, and under TZEN=0 it BusFaults, D1.1")
{
  auto f = booted();
  put(f, secure_code, {0x6001, 0xbe01});
  f.set("r0", vtor + scs_ns);
  f.set("r1", 0x0801'0000);
  REQUIRE(run_at(f, secure_code, 2).reason == stop_reason::user);
  CHECK(f.word(vtor + scs_ns) == 0x0801'0000);
  CHECK(f.word(vtor) != 0x0801'0000);
  auto plain = booted(false);
  put(plain, secure_code, {0x6001, 0xbe01});
  plain.set("r0", vtor + scs_ns);
  plain.set("r1", 0x0801'0000);
  auto const faulted = run_at(plain, secure_code, 2);
  REQUIRE(faulted.reason == stop_reason::fault);
  CHECK(faulted.fault->kind == fault_kind::bus);
  CHECK(faulted.fault->fault_address == vtor + scs_ns);
}

TEST_CASE("the Secure CPACR reaches the engine, so VLSTM stores the frame, B3.36 and C2.4.368")
{
  auto f = booted();
  poke(f, cpacr, 0x00F0'0000);
  CHECK(engine_read(f, UC_ARM_REG_CPACR_S) == 0x00F0'0000);
  REQUIRE(run_vlstm(f).reason == stop_reason::user);
  CHECK(f.word(frame_at) == one_float);
  poke(f, cpacr, 0);
  auto const refused = run_vlstm(f);
  REQUIRE(refused.reason == stop_reason::fault);
  CHECK(refused.fault->kind == fault_kind::usage);
}

auto nonsecure_msr_control_fpca(arch_fixture const& f) -> std::uint32_t
{
  poke(f, sau, sau_allns);
  engine_write(f, UC_ARM_REG_SECURE, 0);
  f.set("control", 0);
  f.set("r0", control_fpca);
  put(f, code_at, {0xf380, 0x8814, 0xbe01});
  REQUIRE(run_at(f, code_at, 2).reason == stop_reason::user);
  return engine_read(f, UC_ARM_REG_CONTROL_S) & control_fpca;
}

TEST_CASE(
    "the Secure NSACR reaches the engine, so a Non-secure MSR CONTROL sets FPCA, D1.2.181 "
    "and D1.2.13")
{
  SUBCASE("NSACR CP10 and CP11")
  {
    auto f = booted();
    poke(f, nsacr, 0x0C00);
    CHECK(engine_read(f, UC_ARM_REG_NSACR) == 0x0C00);
    CHECK(nonsecure_msr_control_fpca(f) == control_fpca);
  }
  SUBCASE("NSACR 0")
  {
    auto f = booted();
    CHECK(engine_read(f, UC_ARM_REG_NSACR) == 0);
    CHECK(nonsecure_msr_control_fpca(f) == 0);
  }
  SUBCASE("a Secure store of NSACR, then BXNS to MSR CONTROL in the same run")
  {
    auto f = booted();
    poke(f, sau_rn, 0);
    poke(f, sau_rb, code_at);
    poke(f, sau_rl, (code_at + 0x1fe0) | sau_enable);
    poke(f, sau, sau_enable);
    f.set("control", 0);
    f.set("r0", control_fpca);
    f.set("r1", nsacr);
    f.set("r2", 0x0C00);
    f.set("r3", code_at);
    put(f, secure_code, {0x600a, 0x471c});
    put(f, code_at, {0xf380, 0x8814, 0xbe01});
    REQUIRE(run_at(f, secure_code, 4).reason == stop_reason::user);
    CHECK(engine_read(f, UC_ARM_REG_SECURE) == 0);
    CHECK(f.word(nsacr) == 0x0C00);
    CHECK((engine_read(f, UC_ARM_REG_CONTROL_S) & control_fpca) == control_fpca);
  }
  SUBCASE("Non-secure stores leave NSACR and the engine value unchanged")
  {
    auto f = booted();
    nonsecure_write(f, nsacr, 0x0C00);
    nonsecure_write(f, nsacr + scs_ns, 0x0C00);
    CHECK(f.word(nsacr) == 0);
    CHECK(engine_read(f, UC_ARM_REG_NSACR) == 0);
    CHECK(nonsecure_msr_control_fpca(f) == 0);
  }
  SUBCASE("a restore into a fresh machine")
  {
    auto f = booted();
    poke(f, nsacr, 0x0C00);
    auto const taken = reread(f.target->snapshot());
    auto       fresh = booted();
    CHECK(engine_read(fresh, UC_ARM_REG_NSACR) == 0);
    fresh.target->restore(taken);
    CHECK(engine_read(fresh, UC_ARM_REG_NSACR) == 0x0C00);
    CHECK(nonsecure_msr_control_fpca(fresh) == control_fpca);
  }
  SUBCASE("a reset")
  {
    auto f = booted();
    poke(f, nsacr, 0x0C00);
    f.target->reset();
    CHECK(f.word(nsacr) == 0);
    CHECK(engine_read(f, UC_ARM_REG_NSACR) == 0);
  }
}

TEST_CASE("FPCCR writes reach the engine by the D1.2.100 table, LSPEN forced 0, LSPACT RAZ/WI")
{
  auto f = booted();
  poke(f, cpacr, 0x00F0'0000);
  SUBCASE("TS")
  {
    poke(f, fpccr, f.word(fpccr) | fpccr_ts);
    CHECK((engine_read(f, UC_ARM_REG_FPCCR_S) & fpccr_ts) != 0);
  }
  SUBCASE("LSPEN")
  {
    poke(f, fpccr, f.word(fpccr) | fpccr_lspen);
    CHECK((engine_read(f, UC_ARM_REG_FPCCR_S) & fpccr_lspen) == 0);
    CHECK((f.word(fpccr) & fpccr_lspen) != 0);
    REQUIRE(run_vlstm(f).reason == stop_reason::user);
    CHECK(f.word(frame_at) == one_float);
  }
  SUBCASE("S")
  {
    REQUIRE(run_vlstm(f).reason == stop_reason::user);
    CHECK((f.word(fpccr) & fpccr_s) != 0);
    poke(f, fpccr, f.word(fpccr) & ~fpccr_s);
    CHECK((f.word(fpccr) & fpccr_s) == 0);
    CHECK((engine_read(f, UC_ARM_REG_FPCCR_S) & fpccr_s) == 0);
    auto fresh = booted();
    fresh.target->restore(reread(f.target->snapshot()));
    CHECK((engine_read(fresh, UC_ARM_REG_FPCCR_S) & fpccr_s) == 0);
  }
  SUBCASE("ASPEN")
  {
    poke(f, fpccr + scs_ns, f.word(fpccr + scs_ns) | fpccr_aspen);
    poke(f, fpccr, f.word(fpccr) & ~fpccr_aspen);
    CHECK((engine_read(f, UC_ARM_REG_FPCCR_NS) & fpccr_aspen) != 0);
    CHECK((engine_read(f, UC_ARM_REG_FPCCR_S) & fpccr_aspen) == 0);
    f.set("control", 0);
    put(f, secure_code, {0xee00, 0x2a10, 0xbe01});
    REQUIRE(run_at(f, secure_code, 2).reason == stop_reason::user);
    CHECK((engine_read(f, UC_ARM_REG_CONTROL_S) & control_fpca) == 0);
    poke(f, sau, sau_allns);
    engine_write(f, UC_ARM_REG_SECURE, 0);
    put(f, code_at, {0xee00, 0x2a10, 0xbe01});
    REQUIRE(run_at(f, code_at, 2).reason == stop_reason::user);
    CHECK((f.reg("control") & control_fpca) != 0);
  }
  SUBCASE("CLRONRET through the Non-secure view")
  {
    poke(f, fpccr, f.word(fpccr) | fpccr_clronrets);
    poke(f, fpccr + scs_ns, f.word(fpccr + scs_ns) | fpccr_clronret);
    CHECK((engine_read(f, UC_ARM_REG_FPCCR_S) & fpccr_clronret) == 0);
    CHECK((f.word(fpccr) & fpccr_clronret) == 0);
    poke(f, fpccr, f.word(fpccr) & ~fpccr_clronrets);
    poke(f, fpccr + scs_ns, f.word(fpccr + scs_ns) | fpccr_clronret);
    CHECK((engine_read(f, UC_ARM_REG_FPCCR_S) & fpccr_clronret) != 0);
    CHECK((engine_read(f, UC_ARM_REG_FPCCR_NS) & fpccr_clronret) == 0);
  }
  SUBCASE("LSPACT")
  {
    poke(f, fpccr, f.word(fpccr) | fpccr_lspact);
    CHECK((f.word(fpccr) & fpccr_lspact) == 0);
    CHECK((engine_read(f, UC_ARM_REG_FPCCR_S) & fpccr_lspact) == 0);
  }
}

TEST_CASE("a restore into a fresh TZEN=1 machine mirrors CPACR_S and FPCCR_S into the engine")
{
  auto f = booted();
  poke(f, cpacr, 0x00F0'0000);
  REQUIRE(run_vlstm(f).reason == stop_reason::user);
  poke(f, fpccr, f.word(fpccr) & ~fpccr_s);
  auto const taken = reread(f.target->snapshot());
  auto       fresh = booted();
  fresh.target->restore(taken);
  CHECK(engine_read(fresh, UC_ARM_REG_CPACR_S) == engine_read(f, UC_ARM_REG_CPACR_S));
  CHECK(engine_read(fresh, UC_ARM_REG_FPCCR_S) == engine_read(f, UC_ARM_REG_FPCCR_S));
  CHECK((engine_read(fresh, UC_ARM_REG_FPCCR_S) & fpccr_lspen) == 0);
  REQUIRE(run_vlstm(fresh).reason == stop_reason::user);
  CHECK(fresh.word(frame_at) == one_float);
}

TEST_CASE("the Secure and Non-secure MPU hold distinct regions, sized by the description")
{
  auto f = booted(true, 4);
  CHECK(f.word(mpu) == 4u << 8);
  CHECK(f.word(mpu + scs_ns) == 8u << 8);
  poke(f, mpu + 8, 3);
  poke(f, mpu + 0xc, 0x2000'0000);
  poke(f, mpu + scs_ns + 8, 3);
  poke(f, mpu + scs_ns + 0xc, 0x2004'0000);
  CHECK(f.word(mpu + 0xc) == 0x2000'0000);
  CHECK(f.word(mpu + scs_ns + 0xc) == 0x2004'0000);
  poke(f, mpu + 8, 6);
  CHECK(f.word(mpu + 8) == 3);
  poke(f, mpu + scs_ns + 8, 6);
  CHECK(f.word(mpu + scs_ns + 8) == 6);
}

TEST_CASE("every banked and Secure-only register round trips through a TZEN=1 snapshot")
{
  constexpr std::array<address, 22> registers{
      vtor,   aircr,       0xE000'ED10, 0xE000'ED14, shpr1,     0xE000'ED1C, 0xE000'ED20, shcsr,
      cpacr,  0xE000'ED8C, mpu + 4,     mpu + 8,     mpu + 0xc, mpu + 0x10,  mpu + 0x30,  sau,
      sau_rn, sau_rb,      sau_rl,      itns0,       fpccr,     syst + 4};
  auto       f     = booted();
  auto const write = [&](std::uint32_t seed) {
    poke(f, aircr, 0x05FA'2000 | (seed & 7u) << 8);
    for(auto const base : {address{0}, scs_ns})
    {
      poke(f, base + vtor, 0x0800'0000 + (seed << 8));
      poke(f, base + 0xE000'ED10, seed & 0x16u);
      poke(f, base + 0xE000'ED14, seed & 0x18u);
      poke(f, base + shpr1, seed * 0x2040'6080u & 0xe0e0'e0e0u);
      poke(f, base + 0xE000'ED1C, seed << 29);
      poke(f, base + 0xE000'ED20, seed * 0x2000'0020u & 0xe0e0'00e0u);
      poke(f, base + shcsr, (seed & 5u) << 16);
      poke(f, base + cpacr, seed & 1u ? 0x00F0'0000 : 0);
      poke(f, base + mpu + 8, seed & 7u);
      poke(f, base + mpu + 0xc, 0x2000'0000 + (seed << 5));
      poke(f, base + mpu + 0x10, 0x2000'1000 + (seed << 5));
      poke(f, base + mpu + 0x30, seed);
      poke(f, base + mpu + 4, seed & 7u);
      poke(f, base + syst + 4, seed);
    }
    poke(f, 0xE000'ED8C, seed & 0xcffu);
    poke(f, sau_rn, seed & 7u);
    poke(f, sau_rb, 0x2000'0000 + (seed << 5));
    poke(f, sau_rl, 0x2000'1000 + (seed << 5));
    poke(f, sau, seed & 3u);
    poke(f, itns0, seed);
    poke(f, fpccr, (seed & 1u ? fpccr_ts : 0u) | fpccr_aspen);
  };
  auto const reads = [&] {
    std::vector<std::uint32_t> out;
    for(auto const base : {address{0}, scs_ns})
    {
      for(auto const at : registers)
      {
        out.push_back(f.word(base + at));
      }
    }
    return out;
  };
  write(0x5a);
  auto const saved = reads();
  auto const taken = reread(f.target->snapshot());
  write(0x25);
  REQUIRE(reads() != saved);
  f.target->restore(taken);
  CHECK(reads() == saved);
}

TEST_CASE("a TZEN=1 snapshot at a Non-secure probe after BLXNS runs on in a fresh machine")
{
  auto f = booted();
  poke(f, sau_rn, 0);
  poke(f, sau_rb, code_at);
  poke(f, sau_rl, (code_at + 0x3e0) | 1u);
  poke(f, sau, sau_enable);
  put(f, code_at, {0xbe01, 0x6801, 0xe840, 0xf200, 0xbe01});
  poke(f, code_at + 0x100, 0x1234'5678);
  put(f, secure_code, {0x47a4, 0xbe01});
  f.set("r4", code_at);
  f.set("r0", code_at + 0x100);
  REQUIRE(run_at(f, secure_code, 2).reason == stop_reason::user);
  REQUIRE(engine_read(f, UC_ARM_REG_SECURE) == 0);
  engine_write(f, UC_ARM_REG_CONTROL_S, engine_read(f, UC_ARM_REG_CONTROL_S) | control_sfpa);
  auto const taken = reread(f.target->snapshot());
  auto       fresh = booted();
  fresh.target->restore(taken);
  CHECK((engine_read(fresh, UC_ARM_REG_CONTROL_S) & control_sfpa) != 0);
  for(auto const id :
      {UC_ARM_REG_SECURE, UC_ARM_REG_MSP_S, UC_ARM_REG_PSP_S, UC_ARM_REG_MSP_NS, UC_ARM_REG_PSP_NS,
       UC_ARM_REG_CONTROL_S, UC_ARM_REG_CONTROL_NS})
  {
    CAPTURE(id);
    CHECK(engine_read(fresh, id) == engine_read(f, id));
  }
  for(auto* const machine : {&f, &fresh})
  {
    auto const stopped = machine->run_to_stop({.max_instructions = icount{3}});
    CHECK(stopped.reason == stop_reason::user);
    CHECK(machine->reg("r1") == 0x1234'5678);
  }
  CHECK(fresh.reg("r2") == f.reg("r2"));
}

TEST_CASE("a TZEN=1 snapshot is refused by a TZEN=0 machine of the same description, key trustzone")
{
  auto       secure = booted();
  auto       plain  = booted(false);
  auto const theirs = reread(secure.target->snapshot());
  auto const own    = reread(plain.target->snapshot());
  auto const pc     = plain.reg("pc");
  try
  {
    plain.target->restore(theirs);
    FAIL("a TZEN=1 snapshot restored into a TZEN=0 machine");
  }
  catch(popcorn::config_error const& refused)
  {
    CHECK(refused.key() == "trustzone");
  }
  CHECK(plain.reg("pc") == pc);
  plain.set("pc", pc + 0x40);
  plain.target->restore(own);
  CHECK(plain.reg("pc") == pc);
}
