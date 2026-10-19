#include "arch_fixture.hpp"

#include "../../../native/src/core/machine_impl.hpp"
#include "cortex_m.hpp"
#include "exceptions.hpp"

#include <popcorn/core/errors.hpp>
#include <popcorn/core/intercept.hpp>
#include <popcorn/core/machine.hpp>

#include <doctest/doctest.h>

#include <unicorn/unicorn.h>

#include <cstdint>
#include <initializer_list>
#include <optional>

namespace
{
namespace core   = popcorn::core;
namespace armv8m = popcorn::arch::armv8m;
using popcorn::address;
using popcorn::icount;
using popcorn::stop_reason;
using popcorn::arch::fault_policy;
using popcorn::testing::arch_fixture;

constexpr address secure_code    = 0x2008'2000;
constexpr address secure_table   = 0x2008'1000;
constexpr address secure_handler = 0x2008'1800;
constexpr address ns_base        = 0x2008'4000;
constexpr address ns_table       = ns_base;
constexpr address ns_handler     = ns_base + 0x200;
constexpr address ns_code        = ns_base + 0x300;
constexpr address ns_stack       = ns_base + 0x800;
constexpr address nsc_base       = 0x0C00'0000;
constexpr address veneer         = nsc_base;

constexpr address vtor   = 0xE000'ED08;
constexpr address iser0  = 0xE000'E100;
constexpr address ispr0  = 0xE000'E200;
constexpr address ipr0   = 0xE000'E400;
constexpr address shcsr  = 0xE000'ED24;
constexpr address cfsr   = 0xE000'ED28;
constexpr address hfsr   = 0xE000'ED2C;
constexpr address sfsr   = 0xE000'EDE4;
constexpr address sfar   = 0xE000'EDE8;
constexpr address sau    = 0xE000'EDD0;
constexpr address sau_rn = 0xE000'EDD8;
constexpr address sau_rb = 0xE000'EDDC;
constexpr address sau_rl = 0xE000'EDE0;
constexpr address scs_ns = 0x0002'0000;

constexpr std::uint16_t sg         = 0xe97f;
constexpr std::uint16_t bkpt       = 0xbe01;
constexpr std::uint16_t nop        = 0xbf00;
constexpr std::uint16_t movs_42    = 0x202a;
constexpr std::uint16_t bx_lr      = 0x4770;
constexpr std::uint16_t bx_r1      = 0x4708;
constexpr std::uint16_t bxns_lr    = 0x4774;
constexpr std::uint16_t blx_r1     = 0x4788;
constexpr std::uint16_t blxns_r3   = 0x479c;
constexpr std::uint16_t push_lr    = 0xb500;
constexpr std::uint16_t pop_pc     = 0xbd00;
constexpr std::uint16_t push_r4_lr = 0xb510;
constexpr std::uint16_t pop_r4_pc  = 0xbd10;
constexpr std::uint16_t ldr_r1     = 0x6801;
constexpr std::uint16_t ldr_pc_a   = 0xf85d;
constexpr std::uint16_t ldr_pc_b   = 0xfb04;
constexpr std::uint16_t mrs_r0_a   = 0xf3ef;
constexpr std::uint16_t mrs_r0_b   = 0x8005;

constexpr std::uint32_t fnc_return       = 0xFEFF'FFFF;
constexpr std::uint32_t sfsr_invep       = 1u << 0;
constexpr std::uint32_t sfsr_auviol      = 1u << 3;
constexpr std::uint32_t sfsr_invtran     = 1u << 4;
constexpr std::uint32_t sfsr_sfarvalid   = 1u << 6;
constexpr std::uint32_t ufsr_invpc       = 1u << 18;
constexpr std::uint32_t ufsr_stkof       = 1u << 20;
constexpr std::uint32_t securefault_ena  = 1u << 19;
constexpr std::uint32_t usagefault_ena   = 1u << 18;
constexpr std::uint32_t hfsr_forced      = 1u << 30;
constexpr std::uint32_t control_sfpa     = 1u << 3;
constexpr std::uint32_t excp_exc_exit    = 8;
constexpr std::uint32_t exc_return_basic = 0xFFFF'FFF9;

auto with_nsc_ram() -> popcorn::part_description
{
  auto desc = popcorn::testing::m33_trustzone();
  desc.regions.push_back({
      .name       = "NSC_RAM",
      .start      = nsc_base,
      .size       = 0x1000,
      .kind       = popcorn::region_kind::ram,
      .acc        = {.read = true, .write = true, .execute = true},
      .secure     = true,
      .physical   = std::nullopt,
      .controller = std::nullopt,
      .content    = std::nullopt
  });
  return desc;
}

auto booted(fault_policy policy = fault_policy::enter_handler) -> arch_fixture
{
  auto options   = popcorn::testing::trustzone_options();
  options.faults = policy;
  arch_fixture f{with_nsc_ram(), "scs", options};
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

void sau_region(arch_fixture const& f, std::uint32_t number, address base, address limit, bool nsc)
{
  poke(f, sau_rn, number);
  poke(f, sau_rb, base);
  poke(f, sau_rl, (limit & ~0x1fu) | (nsc ? 2u : 0u) | 1u);
}

void open_regions(arch_fixture const& f, address nsc_limit = nsc_base + 0x1f)
{
  sau_region(f, 0, ns_base, ns_base + 0x7e0, false);
  sau_region(f, 1, nsc_base, nsc_limit, true);
  poke(f, sau, 1);
  engine_write(f, UC_ARM_REG_MSP_NS, ns_stack);
}

void tables(arch_fixture const& f, std::uint32_t number)
{
  poke(f, vtor, secure_table);
  poke(f, vtor + scs_ns, ns_table);
  poke(f, secure_table + 4 * number, secure_handler | 1u);
  poke(f, ns_table + 4 * number, ns_handler | 1u);
  put(f, secure_handler, {bkpt});
  put(f, ns_handler, {bkpt});
}

auto run_from(arch_fixture const& f, address entry, bool secure, std::uint64_t bound)
    -> popcorn::stop
{
  engine_write(f, UC_ARM_REG_SECURE, secure ? 1u : 0u);
  f.set("pc", entry | 1u);
  return f.run_to_stop({.max_instructions = icount{bound}});
}

auto resume(arch_fixture const& f, std::uint64_t bound) -> popcorn::stop
{
  return f.run_to_stop({.max_instructions = icount{bound}});
}

auto sliced_to_stop(arch_fixture const& f, std::uint64_t slices) -> std::optional<popcorn::stop>
{
  for(std::uint64_t slice = 0; slice < slices; ++slice)
  {
    auto const result = f.run({.max_instructions = icount{1}});
    if(result.stopped and result.stopped->reason != stop_reason::budget)
    {
      return result.stopped;
    }
  }
  return std::nullopt;
}

auto secure_now(arch_fixture const& f) -> bool
{
  return engine_read(f, UC_ARM_REG_SECURE) != 0;
}

auto ipsr(arch_fixture const& f) -> std::uint32_t
{
  return static_cast<std::uint32_t>(f.reg("xpsr")) & 0x1ffu;
}

auto code_hook_count(uc_struct*, std::uint64_t, std::uint32_t, void* user) -> void
{
  ++*static_cast<int*>(user);
}

void secure_function(arch_fixture const& f)
{
  put(f, veneer, {sg, sg, movs_42, bkpt, bxns_lr});
}
}

TEST_CASE(
    "a Non-secure call to an SG veneer enters Secure state, counts SG once, reports the veneer "
    "to its probe, and the engine code hook at the veneer never fires, C2.4.180 and B3.15")
{
  auto f = booted();
  open_regions(f);
  secure_function(f);
  put(f, ns_code, {bkpt, blx_r1, bkpt});
  put(f, secure_code, {bkpt, blx_r1, bkpt});
  auto       engine_hits = 0;
  uc_hook    raw         = 0;
  auto const probe       = f.target->add_probe(veneer);
  REQUIRE(
      uc_hook_add(
          engine_of(f), &raw, UC_HOOK_CODE, reinterpret_cast<void*>(&code_hook_count), &engine_hits,
          veneer, veneer)
      == UC_ERR_OK);

  f.set("r1", veneer | 1u);
  auto const before = run_from(f, ns_code, false, 10);
  REQUIRE(before.reason == stop_reason::user);
  auto const in_secure = resume(f, 10);
  REQUIRE(in_secure.reason == stop_reason::user);
  CHECK(f.reg("pc") == veneer + 8);
  CHECK(secure_now(f));
  CHECK(f.reg("r0") == 42);
  CHECK(f.reg("lr") == ns_code + 4);
  CHECK((engine_read(f, UC_ARM_REG_CONTROL_S) & control_sfpa) == 0);
  CHECK(engine_hits == 0);
  CHECK(probe.hits->count() == 1);
  auto const nonsecure_count = in_secure.instructions.value - before.instructions.value;

  REQUIRE(resume(f, 4).reason == stop_reason::user);
  CHECK_FALSE(secure_now(f));
  CHECK(f.reg("pc") == ns_code + 6);
  CHECK(f.reg("r0") == 42);

  f.set("r1", veneer | 1u);
  auto const from = run_from(f, secure_code, true, 10);
  REQUIRE(from.reason == stop_reason::user);
  auto const control = resume(f, 10);
  REQUIRE(control.reason == stop_reason::user);
  CHECK(f.reg("pc") == veneer + 8);
  CHECK(engine_hits == 1);
  CHECK(probe.hits->count() == 2);
  CHECK(control.instructions.value - from.instructions.value == nonsecure_count);
  REQUIRE(uc_hook_del(engine_of(f), raw) == UC_ERR_OK);
}

TEST_CASE(
    "an SG run in one-instruction slices ends as in one slice, a slice bound right before SG "
    "ends the run there, SG counts as one instruction")
{
  auto const end_of = [](bool sliced) {
    auto f = booted();
    open_regions(f);
    secure_function(f);
    put(f, ns_code, {blx_r1, bkpt});
    auto const probe = f.target->add_probe(veneer);
    f.set("r1", veneer | 1u);
    engine_write(f, UC_ARM_REG_SECURE, 0);
    f.set("pc", ns_code | 1u);
    auto const stopped = sliced ? sliced_to_stop(f, 20) : std::optional{resume(f, 20)};
    REQUIRE(stopped);
    CHECK(stopped->reason == stop_reason::user);
    CHECK(probe.hits->count() == 1);
    return std::tuple{f.reg("pc"), f.reg("r0"), secure_now(f), stopped->instructions.value};
  };
  CHECK(end_of(true) == end_of(false));

  auto f = booted();
  open_regions(f);
  secure_function(f);
  put(f, ns_code, {bkpt, blx_r1, bkpt});
  f.set("r1", veneer | 1u);
  REQUIRE(run_from(f, ns_code, false, 4).reason == stop_reason::user);
  auto const one = f.run({.max_instructions = icount{1}});
  CHECK_FALSE(one.stopped.has_value());
  CHECK(one.executed.value == 1);
  CHECK(f.reg("pc") == veneer);
  CHECK_FALSE(secure_now(f));
  auto const two = f.run({.max_instructions = icount{1}});
  CHECK(two.executed.value == 1);
  CHECK(f.reg("pc") == veneer + 4);
  CHECK(secure_now(f));
}

TEST_CASE(
    "an SG whose second halfword is outside the NSC region is a SecureFault INVEP, B3.15 RJKJD")
{
  for(auto const policy : {fault_policy::stop, fault_policy::enter_handler})
  {
    CAPTURE(policy == fault_policy::stop);
    auto f = booted(policy);
    open_regions(f, nsc_base + 0x1f);
    tables(f, armv8m::exc_securefault);
    poke(f, shcsr, securefault_ena);
    constexpr address split = nsc_base + 0x1e;
    put(f, split, {sg, sg, movs_42, bkpt});
    put(f, ns_code, {blx_r1, bkpt});
    f.set("r1", split | 1u);
    auto const stopped = run_from(f, ns_code, false, 6);
    CHECK((f.word(sfsr) & sfsr_invep) != 0);
    CHECK((f.word(sfsr) & sfsr_sfarvalid) == 0);
    CHECK(f.reg("r0") != 42);
    if(policy == fault_policy::stop)
    {
      REQUIRE(stopped.reason == stop_reason::fault);
      CHECK(stopped.fault->kind == popcorn::fault_kind::secure);
      continue;
    }
    REQUIRE(stopped.reason == stop_reason::user);
    CHECK(f.reg("pc") == secure_handler + 2);
    CHECK(ipsr(f) == armv8m::exc_securefault);
    CHECK(secure_now(f));
  }
}

TEST_CASE(
    "a Non-secure branch to NSC memory holding no SG is a SecureFault INVEP, SFARVALID clear, "
    "B3.15 RJKJD, or HardFault_S FORCED without SECUREFAULTENA")
{
  for(auto const enabled : {false, true})
  {
    CAPTURE(enabled);
    auto f = booted();
    open_regions(f);
    tables(f, armv8m::exc_securefault);
    tables(f, armv8m::exc_hardfault);
    if(enabled)
    {
      poke(f, shcsr, securefault_ena);
    }
    put(f, veneer, {nop, nop, movs_42, bkpt});
    put(f, ns_code, {blx_r1, bkpt});
    f.set("r1", veneer | 1u);
    REQUIRE(run_from(f, ns_code, false, 6).reason == stop_reason::user);
    CHECK(secure_now(f));
    CHECK(f.reg("pc") == secure_handler + 2);
    CHECK(ipsr(f) == (enabled ? armv8m::exc_securefault : armv8m::exc_hardfault));
    CHECK((f.word(hfsr) & hfsr_forced) == (enabled ? 0u : hfsr_forced));
    CHECK((f.word(sfsr) & (sfsr_invep | sfsr_sfarvalid)) == sfsr_invep);
    CHECK((f.word(cfsr) & 0xffu) == 0);
    CHECK(f.reg("r0") != 42);
  }
}

TEST_CASE(
    "a Non-secure branch to Secure memory that is not Non-secure callable is a SecureFault INVEP, "
    "SFARVALID clear, B3.15 RJKJD")
{
  auto f = booted();
  open_regions(f);
  tables(f, armv8m::exc_securefault);
  poke(f, shcsr, securefault_ena);
  put(f, secure_code, {movs_42, bkpt});
  put(f, ns_code, {blx_r1, bkpt});
  f.set("r1", secure_code | 1u);
  REQUIRE(run_from(f, ns_code, false, 6).reason == stop_reason::user);
  CHECK(secure_now(f));
  CHECK(ipsr(f) == armv8m::exc_securefault);
  CHECK(f.reg("pc") == secure_handler + 2);
  CHECK((f.word(sfsr) & (sfsr_invep | sfsr_invtran | sfsr_sfarvalid)) == sfsr_invep);
  CHECK(f.reg("r0") != 42);
}

TEST_CASE("a Secure BX to Non-secure memory with bit 0 set is a SecureFault INVTRAN, B3.15 RXWPC")
{
  for(auto const policy : {fault_policy::stop, fault_policy::enter_handler})
  {
    CAPTURE(policy == fault_policy::stop);
    auto f = booted(policy);
    open_regions(f);
    tables(f, armv8m::exc_securefault);
    poke(f, shcsr, securefault_ena);
    put(f, ns_code, {movs_42, bkpt});
    put(f, secure_code, {bx_r1, bkpt});
    f.set("r1", ns_code | 1u);
    auto const stopped = run_from(f, secure_code, true, 4);
    CHECK((f.word(sfsr) & (sfsr_invtran | sfsr_invep | sfsr_sfarvalid)) == sfsr_invtran);
    CHECK(f.reg("r0") != 42);
    if(policy == fault_policy::stop)
    {
      REQUIRE(stopped.reason == stop_reason::fault);
      CHECK(stopped.fault->kind == popcorn::fault_kind::secure);
      CHECK(stopped.fault->fault_address == ns_code);
      continue;
    }
    REQUIRE(stopped.reason == stop_reason::user);
    CHECK(ipsr(f) == armv8m::exc_securefault);
    CHECK(f.reg("pc") == secure_handler + 2);
  }
}

TEST_CASE(
    "a Non-secure load from Secure memory is a SecureFault AUVIOL with SFAR, B3.14 and D1.2.232")
{
  for(auto const policy : {fault_policy::stop, fault_policy::enter_handler})
  {
    CAPTURE(policy == fault_policy::stop);
    auto f = booted(policy);
    open_regions(f);
    tables(f, armv8m::exc_securefault);
    poke(f, shcsr, securefault_ena);
    constexpr address secret = secure_code + 0x100;
    put(f, ns_code, {ldr_r1, bkpt});
    f.set("r0", secret);
    auto const stopped = run_from(f, ns_code, false, 4);
    CHECK((f.word(sfsr) & (sfsr_auviol | sfsr_sfarvalid)) == (sfsr_auviol | sfsr_sfarvalid));
    CHECK(f.word(sfar) == secret);
    CHECK((f.word(cfsr) & 0xff00u) == 0);
    if(policy == fault_policy::stop)
    {
      REQUIRE(stopped.reason == stop_reason::fault);
      CHECK(stopped.fault->kind == popcorn::fault_kind::secure);
      CHECK(stopped.fault->fault_address == secret);
      continue;
    }
    REQUIRE(stopped.reason == stop_reason::user);
    CHECK(ipsr(f) == armv8m::exc_securefault);
    CHECK(secure_now(f));
  }
}

TEST_CASE(
    "a Non-secure callee of a Secure thread returns through FNC_RETURN in each branch form, "
    "B3.17 RHPFG and RFGNB")
{
  struct form
  {
    char const*                          name;
    std::initializer_list<std::uint16_t> code;
  };
  for(auto const& [name, code] : {
          form{"BX LR",    {movs_42, bx_lr}                      },
          form{"POP {pc}", {movs_42, push_lr, pop_pc}            },
          form{"LDR pc",   {movs_42, push_lr, ldr_pc_a, ldr_pc_b}}
  })
  {
    CAPTURE(name);
    auto f = booted();
    open_regions(f);
    put(f, ns_code, code);
    put(f, secure_code, {blxns_r3, bkpt});
    f.set("r3", ns_code);
    auto const msp_s = engine_read(f, UC_ARM_REG_MSP_S);
    REQUIRE(run_from(f, secure_code, true, 10).reason == stop_reason::user);
    CHECK(secure_now(f));
    CHECK(f.reg("pc") == secure_code + 4);
    CHECK(f.reg("r0") == 42);
    CHECK(ipsr(f) == 0);
    CHECK(engine_read(f, UC_ARM_REG_MSP_S) == msp_s);
    CHECK(engine_read(f, UC_ARM_REG_MSP_NS) == ns_stack);
  }
}

TEST_CASE(
    "BLXNS from a Secure handler shows IPSR 1 to the Non-secure callee, FNC_RETURN restores the "
    "handler's IPSR, the NVIC active bits never change, B3.16 RGVBB and B3.17 RFGNB")
{
  auto f = booted();
  open_regions(f);
  tables(f, 16);
  put(f, secure_handler, {push_r4_lr, blxns_r3, bkpt, pop_r4_pc});
  put(f, ns_code, {mrs_r0_a, mrs_r0_b, bkpt, bx_lr});
  put(f, secure_code, {nop, bkpt});
  f.set("r3", ns_code);
  poke(f, iser0, 1);
  poke(f, ispr0, 1);
  REQUIRE(run_from(f, secure_code, true, 10).reason == stop_reason::user);
  REQUIRE_FALSE(secure_now(f));
  CHECK(f.reg("pc") == ns_code + 6);
  CHECK(f.reg("r0") == 1);
  CHECK(ipsr(f) == 1);
  CHECK(f.module->is_active(16));
  CHECK_FALSE(f.module->thread_mode());
  CHECK_FALSE(f.module->interrupt_returns(excp_exc_exit, exc_return_basic));
  REQUIRE(resume(f, 4).reason == stop_reason::user);
  CHECK(secure_now(f));
  CHECK(f.reg("pc") == secure_handler + 6);
  CHECK(ipsr(f) == 16);
  CHECK(f.module->is_active(16));
  REQUIRE(resume(f, 4).reason == stop_reason::user);
  CHECK(ipsr(f) == 0);
  CHECK(f.reg("pc") == secure_code + 4);
  CHECK_FALSE(f.module->is_active(16));
}

TEST_CASE("interrupt_returns is false at a FNC_RETURN and true at an EXC_RETURN")
{
  auto f = booted();
  open_regions(f);
  tables(f, 16);
  put(f, secure_handler, {blxns_r3, bkpt});
  put(f, ns_code, {bkpt, bx_lr});
  put(f, secure_code, {nop, bkpt});
  f.set("r3", ns_code);
  poke(f, iser0, 1);
  poke(f, ispr0, 1);
  REQUIRE(run_from(f, secure_code, true, 10).reason == stop_reason::user);
  REQUIRE_FALSE(f.module->thread_mode());
  engine_write(f, UC_ARM_REG_PC, fnc_return);
  CHECK_FALSE(f.module->interrupt_returns(excp_exc_exit, exc_return_basic));
  engine_write(f, UC_ARM_REG_PC, exc_return_basic);
  CHECK(f.module->interrupt_returns(excp_exc_exit, exc_return_basic));
}

TEST_CASE(
    "a corrupted RETPSR.Exception is a Secure UsageFault INVPC on FNC_RETURN, the frame kept, "
    "B3.17 RPBNM and FunctionReturn lines 21-28")
{
  for(auto const handler : {false, true})
  {
    for(auto const policy : {fault_policy::stop, fault_policy::enter_handler})
    {
      CAPTURE(handler);
      CAPTURE(policy == fault_policy::stop);
      auto f = booted(policy);
      open_regions(f);
      constexpr address irq_handler = secure_handler + 0x100;
      tables(f, armv8m::exc_usagefault);
      poke(f, secure_table + 4 * 16, irq_handler | 1u);
      put(f, irq_handler, {blxns_r3, bkpt});
      put(f, ns_code, {bkpt, bx_lr});
      put(f, secure_code, {handler ? nop : blxns_r3, bkpt});
      poke(f, shcsr, usagefault_ena);
      f.set("r3", ns_code);
      if(handler)
      {
        poke(f, ipr0, 0x40);
        poke(f, iser0, 1);
        poke(f, ispr0, 1);
      }
      REQUIRE(run_from(f, secure_code, true, 10).reason == stop_reason::user);
      REQUIRE_FALSE(secure_now(f));
      auto const frame = armv8m::read_bank(engine_of(f), armv8m::security::secure).msp;
      poke(f, frame + 4, handler ? 0u : 16u);
      auto const stopped = resume(f, 4);
      CHECK((f.word(cfsr) & ufsr_invpc) != 0);
      CHECK((f.word(cfsr + scs_ns) & ufsr_invpc) == 0);
      if(policy == fault_policy::stop)
      {
        REQUIRE(stopped.reason == stop_reason::fault);
        CHECK(stopped.fault->kind == popcorn::fault_kind::usage);
        CHECK_FALSE(secure_now(f));
        continue;
      }
      REQUIRE(stopped.reason == stop_reason::user);
      CHECK(secure_now(f));
      CHECK(ipsr(f) == armv8m::exc_usagefault);
      CHECK(f.reg("pc") == secure_handler + 2);
    }
  }
}

TEST_CASE(
    "BX LR and BXNS LR to FNC_RETURN in Secure state return through the Secure stack frame, "
    "FunctionReturn in either state")
{
  for(auto const insn : {bx_lr, bxns_lr})
  {
    CAPTURE(insn);
    auto f = booted();
    open_regions(f);
    constexpr address back = secure_code + 0x40;
    put(f, back, {movs_42, bkpt});
    put(f, secure_code, {insn, bkpt});
    auto const msp_s = engine_read(f, UC_ARM_REG_MSP_S);
    poke(f, msp_s - 8, back | 1u);
    poke(f, msp_s - 4, 0);
    engine_write(f, UC_ARM_REG_MSP_S, msp_s - 8);
    f.set("lr", fnc_return);
    REQUIRE(run_from(f, secure_code, true, 6).reason == stop_reason::user);
    CHECK(secure_now(f));
    CHECK(f.reg("pc") == back + 4);
    CHECK(f.reg("r0") == 42);
    CHECK(engine_read(f, UC_ARM_REG_MSP_S) == msp_s);
  }
}

TEST_CASE(
    "a FNC_RETURN frame not 8-byte aligned is a Secure UsageFault INVPC, the UNPREDICTABLE case "
    "of FunctionReturn line 11")
{
  for(auto const policy : {fault_policy::stop, fault_policy::enter_handler})
  {
    CAPTURE(policy == fault_policy::stop);
    auto f = booted(policy);
    open_regions(f);
    tables(f, armv8m::exc_usagefault);
    poke(f, shcsr, usagefault_ena);
    put(f, secure_code, {bx_lr, bkpt});
    auto const msp_s = engine_read(f, UC_ARM_REG_MSP_S);
    poke(f, msp_s - 12, (secure_code + 0x40) | 1u);
    poke(f, msp_s - 8, 0);
    engine_write(f, UC_ARM_REG_MSP_S, msp_s - 12);
    f.set("lr", fnc_return);
    auto const stopped = run_from(f, secure_code, true, 4);
    CHECK((f.word(cfsr) & ufsr_invpc) != 0);
    CHECK((f.word(cfsr + scs_ns) & ufsr_invpc) == 0);
    if(policy == fault_policy::stop)
    {
      REQUIRE(stopped.reason == stop_reason::fault);
      CHECK(stopped.fault->kind == popcorn::fault_kind::usage);
      CHECK(engine_read(f, UC_ARM_REG_MSP_S) == msp_s - 12);
      continue;
    }
    REQUIRE(stopped.reason == stop_reason::user);
    CHECK(secure_now(f));
    CHECK(ipsr(f) == armv8m::exc_usagefault);
    CHECK(f.reg("pc") == secure_handler + 2);
  }
}

TEST_CASE(
    "a FNC_RETURN frame that cannot be read is a BusFault UNSTKERR, or HardFault_S FORCED without "
    "BUSFAULTENA, raised with the branch, FunctionReturn lines 14-15")
{
  constexpr address       unreadable    = 0x6000'0000;
  constexpr std::uint32_t busfault_ena  = 1u << 17;
  constexpr std::uint32_t bfsr_unstkerr = 1u << 11;
  for(auto const policy : {fault_policy::stop, fault_policy::enter_handler})
  {
    for(auto const enabled : {true, false})
    {
      CAPTURE(policy == fault_policy::stop);
      CAPTURE(enabled);
      auto f = booted(policy);
      open_regions(f);
      tables(f, armv8m::exc_busfault);
      tables(f, armv8m::exc_hardfault);
      if(enabled)
      {
        poke(f, shcsr, busfault_ena);
      }
      put(f, ns_code, {bx_lr, bkpt});
      engine_write(f, UC_ARM_REG_MSP_S, unreadable);
      f.set("lr", fnc_return);
      auto const stopped = run_from(f, ns_code, false, 4);
      CHECK((f.word(cfsr) & bfsr_unstkerr) != 0);
      CHECK(engine_read(f, UC_ARM_REG_MSP_S) == unreadable);
      if(policy == fault_policy::stop)
      {
        REQUIRE(stopped.reason == stop_reason::fault);
        CHECK(stopped.fault->kind == popcorn::fault_kind::bus);
        CHECK_FALSE(secure_now(f));
        CHECK(engine_read(f, UC_ARM_REG_MSP_NS) == ns_stack);
        continue;
      }
      REQUIRE(stopped.reason == stop_reason::user);
      CHECK(secure_now(f));
      CHECK(ipsr(f) == (enabled ? armv8m::exc_busfault : armv8m::exc_hardfault));
      CHECK(((f.word(hfsr) & hfsr_forced) != 0) != enabled);
      CHECK(f.reg("pc") == secure_handler + 2);
      CHECK(engine_read(f, UC_ARM_REG_MSP_NS) == ns_stack - 0x20);
      CHECK(f.reg("lr") == 0xFFFF'FFB9u);
    }
  }
}

TEST_CASE(
    "BLXNS with MSPLIM_S above its frame is a Secure UsageFault STKOF, B3.21 RCCSC, whose entry "
    "takes it with MSP_S at the limit, DerivedLateArrival lines 38-50")
{
  for(auto const policy : {fault_policy::stop, fault_policy::enter_handler})
  {
    CAPTURE(policy == fault_policy::stop);
    auto f = booted(policy);
    open_regions(f);
    tables(f, armv8m::exc_usagefault);
    poke(f, shcsr, usagefault_ena);
    put(f, ns_code, {movs_42, bkpt});
    put(f, secure_code, {blxns_r3, bkpt});
    f.set("r3", ns_code);
    auto const msp_s = engine_read(f, UC_ARM_REG_MSP_S);
    engine_write(f, UC_ARM_REG_MSPLIM_S, msp_s);
    auto const stopped = run_from(f, secure_code, true, 4);
    CHECK((f.word(cfsr) & ufsr_stkof) != 0);
    CHECK((f.word(cfsr + scs_ns) & ufsr_stkof) == 0);
    CHECK(f.reg("r0") != 42);
    if(policy == fault_policy::stop)
    {
      REQUIRE(stopped.reason == stop_reason::fault);
      CHECK(stopped.fault->kind == popcorn::fault_kind::usage);
      continue;
    }
    REQUIRE(stopped.reason == stop_reason::user);
    CHECK_FALSE(f.module->is_pending(armv8m::exc_usagefault));
    CHECK(secure_now(f));
    CHECK(ipsr(f) == armv8m::exc_usagefault);
    CHECK(f.reg("pc") == secure_handler + 2);
    CHECK(engine_read(f, UC_ARM_REG_MSP_S) == msp_s);
  }
}

TEST_CASE(
    "an intercept at an SG veneer returns its value to the Non-secure caller without entering "
    "Secure state, in one slice and in one-instruction slices")
{
  for(auto const sliced : {false, true})
  {
    CAPTURE(sliced);
    auto f = booted();
    open_regions(f);
    secure_function(f);
    put(f, ns_code, {blx_r1, bkpt});
    auto       calls = 0;
    auto const hook  = f.target->add_intercept(veneer, [&calls](core::intercept_call const&) {
      ++calls;
      return core::intercept_result::returns(popcorn::arch::call_arg{.bits = 7});
    });
    auto const body  = f.target->add_probe(veneer + 4);
    f.set("r1", veneer | 1u);
    engine_write(f, UC_ARM_REG_SECURE, 0);
    f.set("pc", ns_code | 1u);
    auto const stopped = sliced ? sliced_to_stop(f, 20) : std::optional{resume(f, 20)};
    REQUIRE(stopped);
    CHECK(stopped->reason == stop_reason::user);
    CHECK(f.reg("pc") == ns_code + 4);
    CHECK(f.reg("r0") == 7);
    CHECK_FALSE(secure_now(f));
    CHECK(calls == 1);
    CHECK(body.hits->count() == 0);
  }
}

TEST_CASE("an intercept stop at an SG veneer stops before SG, the resume enters once")
{
  auto f = booted();
  open_regions(f);
  secure_function(f);
  put(f, ns_code, {bkpt, blx_r1, bkpt});
  auto const probe = f.target->add_probe(veneer);
  auto const hook  = f.target->add_intercept(
      veneer,
      [](core::intercept_call const&) { return core::intercept_result::stop(stop_reason::user); },
      core::intercept_mode::once);
  f.set("r1", veneer | 1u);
  auto const before = run_from(f, ns_code, false, 10);
  REQUIRE(before.reason == stop_reason::user);
  auto const stopped = resume(f, 10);
  REQUIRE(stopped.reason == stop_reason::user);
  CHECK(f.reg("pc") == veneer);
  CHECK_FALSE(secure_now(f));
  CHECK(stopped.instructions.value - before.instructions.value == 1);
  REQUIRE(resume(f, 10).reason == stop_reason::user);
  CHECK(secure_now(f));
  CHECK(f.reg("pc") == veneer + 8);
  CHECK(probe.hits->count() == 1);
}

TEST_CASE(
    "an intercept returning at an SG veneer to a FNC_RETURN link completes the function return "
    "in the trap, without a nested engine interrupt")
{
  auto f = booted();
  open_regions(f);
  secure_function(f);
  put(f, ns_code, {bx_r1});
  put(f, secure_code, {blxns_r3, bkpt});
  auto const hook = f.target->add_intercept(veneer, [](core::intercept_call const&) {
    return core::intercept_result::returns(popcorn::arch::call_arg{.bits = 7});
  });
  f.set("r1", veneer | 1u);
  f.set("r3", ns_code);
  REQUIRE(run_from(f, secure_code, true, 10).reason == stop_reason::user);
  CHECK(secure_now(f));
  CHECK(f.reg("pc") == secure_code + 4);
  CHECK(f.reg("r0") == 7);
}

TEST_CASE(
    "an intercept returning from a Non-secure function a BLXNS reached returns through "
    "FNC_RETURN, in thread and handler mode")
{
  for(auto const handler : {false, true})
  {
    CAPTURE(handler);
    auto f = booted();
    open_regions(f);
    tables(f, 16);
    put(f, ns_code, {movs_42, bkpt});
    put(f, secure_handler, {blxns_r3, bkpt});
    put(f, secure_code, {handler ? nop : blxns_r3, bkpt});
    auto const hook = f.target->add_intercept(ns_code, [](core::intercept_call const&) {
      return core::intercept_result::returns(popcorn::arch::call_arg{.bits = 5});
    });
    f.set("r3", ns_code);
    if(handler)
    {
      poke(f, iser0, 1);
      poke(f, ispr0, 1);
    }
    REQUIRE(run_from(f, secure_code, true, 10).reason == stop_reason::user);
    CHECK(secure_now(f));
    CHECK(f.reg("r0") == 5);
    CHECK(f.reg("pc") == (handler ? secure_handler : secure_code) + 4);
    CHECK(ipsr(f) == (handler ? 16u : 0u));
  }
}

TEST_CASE("a call refuses an entry of the other security state, before any state changes")
{
  auto f = booted();
  open_regions(f);
  put(f, ns_code, {movs_42, bx_lr});
  put(f, secure_code, {movs_42, bx_lr});
  popcorn::arch::call_frame const secure_call{
      .entry = secure_code, .return_sentinel = secure_code + 0x400};
  CHECK_NOTHROW(f.module->check_call(secure_call));
  auto nonsecure_call  = secure_call;
  nonsecure_call.entry = ns_code;
  auto const pc_before = f.reg("pc");
  try
  {
    std::ignore = f.target->call(nonsecure_call, icount{20});
    FAIL("the call ran");
  }
  catch(popcorn::config_error const& error)
  {
    CHECK(error.key() == "entry");
    CHECK(std::string_view{error.what()}.find("0x20084300") != std::string_view::npos);
    CHECK(std::string_view{error.what()}.find("Non-secure") != std::string_view::npos);
  }
  CHECK_THROWS_AS(f.module->check_call(nonsecure_call), popcorn::config_error);
  CHECK(f.reg("pc") == pc_before);
  CHECK_FALSE(f.target->in_call());
  auto const returned = f.target->call(secure_call, icount{20});
  CHECK(returned.value == 42);
}

TEST_CASE(
    "one machine and one arena return a Non-secure and a Secure call, the Secure one through a "
    "fault on its Non-secure sentinel, and refuse a Non-secure call with a Secure arena part")
{
  constexpr address arena_base = 0x6000'0000;
  constexpr address sentinel   = arena_base + 0xff0;
  auto              options    = popcorn::testing::trustzone_options();
  options.scratch = popcorn::address_range{.name = "arena", .start = arena_base, .size = 0x1000};
  arch_fixture f{with_nsc_ram(), "scs", options};
  REQUIRE(f.run_to_stop().reason == stop_reason::user);
  open_regions(f);
  put(f, ns_code, {movs_42, bx_lr});
  put(f, secure_code, {0x202b, bx_lr});
  popcorn::arch::call_frame const secure_call{.entry = secure_code, .return_sentinel = sentinel};
  popcorn::arch::call_frame const ns_call{.entry = ns_code, .return_sentinel = sentinel};
  CHECK_NOTHROW(f.module->check_call(secure_call));

  engine_write(f, UC_ARM_REG_SECURE, 0);
  try
  {
    f.module->check_call(ns_call);
    FAIL("the frame passed");
  }
  catch(popcorn::config_error const& error)
  {
    CHECK(error.key() == "arena");
    CHECK(std::string_view{error.what()}.find("0x60000000") != std::string_view::npos);
  }
  sau_region(f, 2, arena_base, arena_base + 0x7e0, false);
  CHECK_THROWS_AS(f.module->check_call(ns_call), popcorn::config_error);
  sau_region(f, 2, arena_base, arena_base + 0xfe0, false);
  auto const ns_returned = f.target->call(ns_call, icount{20});
  CHECK(ns_returned.stopped.reason == stop_reason::returned);
  CHECK(ns_returned.value == 42);
  CHECK_FALSE(secure_now(f));

  engine_write(f, UC_ARM_REG_SECURE, 1);
  CHECK_NOTHROW(f.module->check_call(secure_call));
  auto const secure_returned = f.target->call(secure_call, icount{20});
  CHECK(secure_returned.stopped.reason == stop_reason::returned);
  CHECK(secure_returned.value == 43);
  CHECK(secure_now(f));
  CHECK((f.word(sfsr) & sfsr_invtran) == 0);
}
