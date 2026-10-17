#include "arch_fixture.hpp"

#include "../../../native/src/core/machine_impl.hpp"
#include "cortex_m.hpp"
#include "exceptions.hpp"
#include "frame.hpp"
#include "state.hpp"

#include <popcorn/core/machine.hpp>

#include <doctest/doctest.h>

#include <reflex/serde/bson.hpp>

#include <unicorn/unicorn.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <initializer_list>
#include <random>
#include <ranges>
#include <tuple>
#include <vector>

namespace
{
namespace core   = popcorn::core;
namespace armv8m = popcorn::arch::armv8m;
using armv8m::security;
using popcorn::address;
using popcorn::icount;
using popcorn::stop_reason;
using popcorn::arch::fault_policy;
using popcorn::testing::arch_fixture;
using popcorn::testing::m33_trustzone;
using popcorn::testing::trustzone_options;

constexpr address secure_code    = 0x2008'2000;
constexpr address secure_table   = 0x2008'1000;
constexpr address secure_handler = 0x2008'1800;
constexpr address ns_base        = 0x2008'4000;
constexpr address ns_table       = ns_base;
constexpr address ns_handler     = ns_base + 0x200;
constexpr address ns_code        = ns_base + 0x300;
constexpr address ns_stack       = ns_base + 0x800;
constexpr address unmapped       = 0x6000'0000;

constexpr address vtor   = 0xE000'ED08;
constexpr address aircr  = 0xE000'ED0C;
constexpr address iser0  = 0xE000'E100;
constexpr address ispr0  = 0xE000'E200;
constexpr address icsr   = 0xE000'ED04;
constexpr address scr    = 0xE000'ED10;
constexpr address sfsr   = 0xE000'EDE4;
constexpr address sfar   = 0xE000'EDE8;
constexpr address shcsr  = 0xE000'ED24;
constexpr address cpacr  = 0xE000'ED88;
constexpr address fpccr  = 0xE000'EF34;
constexpr address itns0  = 0xE000'E380;
constexpr address cfsr   = 0xE000'ED28;
constexpr address hfsr   = 0xE000'ED2C;
constexpr address sau    = 0xE000'EDD0;
constexpr address sau_rn = 0xE000'EDD8;
constexpr address sau_rb = 0xE000'EDDC;
constexpr address sau_rl = 0xE000'EDE0;
constexpr address scs_ns = 0x0002'0000;

constexpr std::uint32_t bfhfnmins = 0x05FA'2000;
constexpr std::uint32_t pris      = 0x05FA'4000;
constexpr std::uint16_t bkpt      = 0xbe01;
constexpr std::uint16_t udf       = 0xde00;
constexpr std::uint16_t ldr_r1_r0 = 0x6801;
constexpr std::uint16_t bx_lr     = 0x4770;
constexpr std::uint16_t nop       = 0xbf00;

constexpr std::uint32_t control_spsel = 1u << 1;
constexpr std::uint32_t control_fpca  = 1u << 2;
constexpr std::uint32_t fill          = 0xA5A5'A5A5;

auto booted(fault_policy policy = fault_policy::enter_handler, char const* firmware = "scs")
    -> arch_fixture
{
  auto options   = trustzone_options();
  options.faults = policy;
  arch_fixture f{m33_trustzone(), firmware, options};
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

auto saved(arch_fixture const& f) -> armv8m::state
{
  std::vector<std::byte> bytes;
  f.module->save(bytes);
  return reflex::serde::bson::deserializer{bytes}.load<armv8m::state>();
}

void seed(arch_fixture const& f, auto&& edit)
{
  auto state = saved(f);
  edit(state);
  std::vector<std::byte> edited;
  reflex::serde::bson::serializer{edited}.dump(state);
  f.module->load(edited);
}

void open_nonsecure_region(arch_fixture const& f)
{
  poke(f, sau_rn, 0);
  poke(f, sau_rb, ns_base);
  poke(f, sau_rl, (ns_base + 0x7e0) | 1u);
  poke(f, sau, 1);
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

auto run_secure(arch_fixture const& f, address entry, std::uint64_t bound) -> popcorn::stop
{
  f.set("pc", entry | 1u);
  return f.run_to_stop({.max_instructions = icount{bound}});
}

auto run_nonsecure(arch_fixture const& f, address entry, std::uint64_t bound) -> popcorn::stop
{
  engine_write(f, UC_ARM_REG_MSP_NS, ns_stack);
  engine_write(f, UC_ARM_REG_SECURE, 0);
  f.set("pc", entry | 1u);
  return f.run_to_stop({.max_instructions = icount{bound}});
}

auto resume(arch_fixture const& f, std::uint64_t bound) -> popcorn::stop
{
  return f.run_to_stop({.max_instructions = icount{bound}});
}

auto secure_now(arch_fixture const& f) -> bool
{
  return engine_read(f, UC_ARM_REG_SECURE) != 0;
}

void set_registers(arch_fixture const& f)
{
  for(int n = 0; n <= 12; ++n)
  {
    f.set(std::format("r{}", n), 0x100u + static_cast<std::uint32_t>(n));
  }
}

auto ipsr(arch_fixture const& f) -> std::uint32_t
{
  return static_cast<std::uint32_t>(f.reg("xpsr")) & 0x1ffu;
}

auto exceptions_with(std::uint32_t irqs) -> armv8m::exception_state
{
  auto const count = 16 + irqs;
  auto const words = (count + 31) / 32;
  return {
      .priority          = std::vector<std::uint8_t>(count, 0),
      .enabled           = std::vector<std::uint32_t>(words, 0),
      .pending           = std::vector<std::uint32_t>(words, 0),
      .active            = std::vector<std::uint32_t>(words, 0),
      .level             = std::vector<std::uint32_t>(words, 0),
      .targets_nonsecure = std::vector<std::uint32_t>(words, 0),
  };
}

constexpr armv8m::security_config tz{.trustzone = true};

auto with(armv8m::security_config config, auto&& edit) -> armv8m::security_config
{
  edit(config);
  return config;
}

auto secure_if(bool value) -> security
{
  return value ? security::secure : security::nonsecure;
}
}

TEST_CASE("exception_target follows ExceptionTargetsSecure(), B3.11 RJQMQ and RDXYK")
{
  auto exceptions = exceptions_with(8);
  for(auto const bf : {false, true})
  {
    for(auto const sdme : {false, true})
    {
      for(auto const itns : {false, true})
      {
        for(auto const origin : {security::secure, security::nonsecure})
        {
          CAPTURE(bf);
          CAPTURE(sdme);
          CAPTURE(itns);
          CAPTURE(origin == security::secure);
          armv8m::put_bit(exceptions.targets_nonsecure, 17, itns);
          auto const config = with(tz, [&](armv8m::security_config& c) {
            c.bfhfnmins = bf;
            c.sdme      = sdme;
          });
          auto const target = [&](std::uint32_t number) {
            return armv8m::exception_target(exceptions, config, number, origin);
          };
          CHECK(target(armv8m::exc_nmi) == secure_if(not bf));
          CHECK(target(armv8m::exc_hardfault) == secure_if(not bf or origin == security::secure));
          CHECK(target(armv8m::exc_memmanage) == origin);
          CHECK(target(armv8m::exc_busfault) == secure_if(not bf));
          CHECK(target(armv8m::exc_usagefault) == origin);
          CHECK(target(armv8m::exc_securefault) == security::secure);
          CHECK(target(armv8m::exc_svcall) == origin);
          CHECK(target(armv8m::exc_debugmon) == secure_if(sdme));
          CHECK(target(armv8m::exc_pendsv) == origin);
          CHECK(target(armv8m::exc_systick) == origin);
          CHECK(target(17) == secure_if(not itns));
          for(std::uint32_t number = armv8m::exc_nmi; number < 24; ++number)
          {
            CHECK(armv8m::exception_target(exceptions, {}, number, origin) == security::secure);
          }
        }
      }
    }
  }
}

TEST_CASE("exception_priority follows ExceptionPriority(), HardFault_S -3 with BFHFNMINS, B3.9")
{
  auto exceptions                                    = exceptions_with(8);
  exceptions.priority[armv8m::exc_systick]           = 0x48;
  exceptions.nonsecure_priority[armv8m::exc_systick] = 0x48;
  exceptions.priority[17]                            = 0x48;
  auto const bf  = with(tz, [](auto& c) { c.bfhfnmins = true; });
  auto const pri = [&](armv8m::security_config const& c, std::uint32_t n, security s, bool g) {
    return armv8m::exception_priority(exceptions, c, n, s, g);
  };
  CHECK(pri(tz, armv8m::exc_hardfault, security::secure, true) == -1);
  CHECK(pri(bf, armv8m::exc_hardfault, security::secure, true) == -3);
  CHECK(pri(bf, armv8m::exc_hardfault, security::nonsecure, true) == -1);
  CHECK(pri(tz, armv8m::exc_nmi, security::secure, true) == -2);
  CHECK(pri(bf, armv8m::exc_nmi, security::nonsecure, true) == -2);
  auto const grouped = with(tz, [](auto& c) {
    c.prigroup           = 5;
    c.prigroup_nonsecure = 4;
  });
  CHECK(pri(grouped, armv8m::exc_systick, security::secure, true) == 0x40);
  CHECK(pri(grouped, armv8m::exc_systick, security::nonsecure, true) == 0x40);
  CHECK(pri(grouped, armv8m::exc_systick, security::nonsecure, false) == 0x48);
  auto const restricted = with(grouped, [](auto& c) { c.pris = true; });
  CHECK(pri(restricted, armv8m::exc_systick, security::nonsecure, true) == 0xA0);
  CHECK(pri(restricted, armv8m::exc_systick, security::nonsecure, false) == 0xA4);
  CHECK(pri(restricted, armv8m::exc_systick, security::secure, true) == 0x40);
  CHECK(pri(restricted, 17, security::nonsecure, true) == 0xA0);
  CHECK(pri(restricted, armv8m::exc_hardfault, security::nonsecure, true) == -1);
}

TEST_CASE("execution_priority follows ExecutionPriority() lines 9-56 for both banks' masks")
{
  auto       exceptions = exceptions_with(8);
  auto const bf         = with(tz, [](auto& c) { c.bfhfnmins = true; });
  auto const restricted = with(tz, [](auto& c) { c.pris = true; });
  auto const run        = [&](armv8m::security_config const& c, armv8m::masks s, armv8m::masks ns) {
    return armv8m::execution_priority(exceptions, c, s, ns);
  };
  CHECK(run(tz, {}, {}) == 256);
  CHECK(run(tz, {.faultmask = true}, {}) == -1);
  CHECK(run(bf, {.faultmask = true}, {}) == -3);
  CHECK(run(tz, {}, {.faultmask = true}) == 0);
  CHECK(run(restricted, {}, {.faultmask = true}) == 0x80);
  CHECK(run(bf, {}, {.faultmask = true}) == -1);
  CHECK(run(tz, {}, {.primask = true}) == 0);
  CHECK(run(restricted, {}, {.primask = true}) == 0x80);
  CHECK(run(restricted, {.primask = true}, {}) == 0);
  CHECK(run(tz, {}, {.basepri = 0x40}) == 0x40);
  CHECK(run(restricted, {}, {.basepri = 0x40}) == 0xA0);
  CHECK(run(restricted, {.basepri = 0x90}, {.basepri = 0x40}) == 0x90);
  auto const ns_grouped = with(tz, [](auto& c) { c.prigroup_nonsecure = 5; });
  CHECK(run(ns_grouped, {}, {.basepri = 0x48}) == 0x40);
  exceptions.nonsecure_priority[armv8m::exc_pendsv] = 0x20;
  exceptions.nonsecure_active                       = 1u << armv8m::exc_pendsv;
  CHECK(run(tz, {}, {}) == 0x20);
  CHECK(run(restricted, {}, {}) == 0x90);
  CHECK(run({}, {}, {}) == 256);
  armv8m::put_bit(exceptions.active, armv8m::exc_hardfault, true);
  CHECK(run(bf, {}, {}) == -3);
  CHECK(run(tz, {}, {}) == -1);
  exceptions.active[0]        = 0;
  exceptions.nonsecure_active = 1u << armv8m::exc_hardfault;
  CHECK(run(bf, {}, {}) == -1);
}

TEST_CASE("without TrustZone the banked execution_priority equals the one-bank one")
{
  std::mt19937_64 random{0x0600'a001u};
  for(int round = 0; round < 2000; ++round)
  {
    auto exceptions = exceptions_with(40);
    for(auto& byte : exceptions.priority)
    {
      byte = static_cast<std::uint8_t>(random() & 0xf0u);
    }
    for(auto& word : exceptions.active)
    {
      word = static_cast<std::uint32_t>(random()) & static_cast<std::uint32_t>(random());
    }
    exceptions.nonsecure_active  = static_cast<std::uint32_t>(random());
    auto const          prigroup = static_cast<std::uint32_t>(random() % 8);
    armv8m::masks const held{
        .primask   = (random() & 7u) == 0,
        .basepri   = static_cast<std::uint8_t>((random() & 1u) != 0 ? random() & 0xf0u : 0u),
        .faultmask = (random() & 7u) == 0,
    };
    armv8m::masks const other{.primask = true, .basepri = 0x10, .faultmask = true};
    CAPTURE(round);
    CHECK(
        armv8m::execution_priority(exceptions, {.prigroup = prigroup}, held, other)
        == armv8m::execution_priority(exceptions, prigroup, held));
  }
}

TEST_CASE("without TrustZone EXC_RETURN keeps S, DCRS and ES set for the six entry cases")
{
  using armv8m::encode_exc_return;
  CHECK(encode_exc_return({.thread = true, .basic_frame = true}, {}) == 0xFFFFFFF9);
  CHECK(
      encode_exc_return({.thread = true, .process_stack = true, .basic_frame = true}, {})
      == 0xFFFFFFFD);
  CHECK(encode_exc_return({.basic_frame = true}, {}) == 0xFFFFFFF1);
  CHECK(encode_exc_return({.thread = true}, {}) == 0xFFFFFFE9);
  CHECK(encode_exc_return({.thread = true, .process_stack = true}, {}) == 0xFFFFFFED);
  CHECK(encode_exc_return({}, {}) == 0xFFFFFFE1);
  CHECK(
      encode_exc_return(
          {.thread = true, .basic_frame = true},
          {.secure_background = true, .secure_exception = false})
      == 0xFFFFFFF8);
  CHECK(
      armv8m::decode_exc_security(0xFFFFFFB8)
      == armv8m::exc_return_security{
          .secure_background = false, .default_callee = true, .secure_exception = false});
}

TEST_CASE("best_pending over both banks ranks by the target bank's priority, then number, B3.13")
{
  auto exceptions = exceptions_with(8);
  armv8m::put_bit(exceptions.enabled, 17, true);
  armv8m::put_bit(exceptions.pending, 17, true);
  exceptions.priority[17]                            = 0x20;
  exceptions.nonsecure_pending                       = 1u << armv8m::exc_systick;
  exceptions.nonsecure_priority[armv8m::exc_systick] = 0x10;
  CHECK(
      armv8m::best_pending(exceptions, tz)
      == armv8m::pending_exception{.number = armv8m::exc_systick, .state = security::nonsecure});
  auto const restricted = with(tz, [](auto& c) { c.pris = true; });
  CHECK(
      armv8m::best_pending(exceptions, restricted)
      == armv8m::pending_exception{.number = 17, .state = security::secure});
  armv8m::put_bit(exceptions.targets_nonsecure, 17, true);
  exceptions.priority[17] = 0x10;
  CHECK(armv8m::best_pending(exceptions, tz).number == armv8m::exc_systick);
  CHECK(
      armv8m::best_pending(exceptions, restricted)
      == armv8m::pending_exception{.number = armv8m::exc_systick, .state = security::nonsecure});
  exceptions.nonsecure_pending = 0;
  CHECK(
      armv8m::best_pending(exceptions, tz)
      == armv8m::pending_exception{.number = 17, .state = security::nonsecure});
  armv8m::put_bit(exceptions.pending, armv8m::exc_systick, true);
  exceptions.priority[armv8m::exc_systick]           = 0x10;
  exceptions.nonsecure_pending                       = 1u << armv8m::exc_systick;
  exceptions.nonsecure_priority[armv8m::exc_systick] = 0x10;
  CHECK(
      armv8m::best_pending(exceptions, tz)
      == armv8m::pending_exception{.number = armv8m::exc_systick, .state = security::secure});
  exceptions.nonsecure_pending                          = 1u << armv8m::exc_usagefault;
  exceptions.nonsecure_priority[armv8m::exc_usagefault] = 0x10;
  CHECK(
      armv8m::best_pending(exceptions, tz)
      == armv8m::pending_exception{.number = armv8m::exc_usagefault, .state = security::nonsecure});
  CHECK(
      armv8m::best_pending(exceptions, tz, true)
      == armv8m::pending_exception{.number = armv8m::exc_usagefault, .state = security::nonsecure});
  CHECK(armv8m::best_pending(exceptions, armv8m::security_config{}).number == armv8m::exc_systick);
  CHECK(armv8m::any_nonsecure_pending(exceptions));
  CHECK_FALSE(armv8m::any_nonsecure_pending(exceptions_with(8)));
}

TEST_CASE("a Non-secure IRQ at 0 does not preempt a Secure handler at 0x40 with PRIS, B3.13")
{
  auto f = booted();
  seed(f, [](armv8m::state& s) {
    armv8m::put_bit(s.exceptions.active, 17, true);
    s.exceptions.priority[17] = 0x40;
  });
  poke(f, itns0, 1u << 2);
  poke(f, iser0, 1u << 2);
  CHECK(f.module->would_preempt(18));
  poke(f, aircr, pris);
  CHECK_FALSE(f.module->would_preempt(18));
  poke(f, itns0, 0);
  CHECK(f.module->would_preempt(18));
}

TEST_CASE("FAULTMASK_S and an active HardFault_S hold the execution priority at -3 with BFHFNMINS")
{
  auto f = booted();
  engine_write(f, UC_ARM_REG_FAULTMASK_S, 1);
  REQUIRE(engine_read(f, UC_ARM_REG_FAULTMASK_S) == 1);
  CHECK(f.module->would_preempt(armv8m::exc_nmi));
  poke(f, aircr, bfhfnmins);
  CHECK_FALSE(f.module->would_preempt(armv8m::exc_nmi));
  engine_write(f, UC_ARM_REG_FAULTMASK_S, 0);
  CHECK(f.module->would_preempt(armv8m::exc_nmi));
  seed(f, [](armv8m::state& s) {
    armv8m::put_bit(s.exceptions.active, armv8m::exc_hardfault, true);
  });
  CHECK_FALSE(f.module->would_preempt(armv8m::exc_nmi));
  poke(f, aircr, 0x05FA'0000);
  CHECK(f.module->would_preempt(armv8m::exc_nmi));
}

TEST_CASE("a Secure UsageFault escalates to HardFault_S with BFHFNMINS=1, B3.11 RDXYK")
{
  auto f = booted();
  poke(f, aircr, bfhfnmins);
  tables(f, armv8m::exc_hardfault);
  put(f, secure_code, {udf});
  REQUIRE(run_secure(f, secure_code, 3).reason == stop_reason::user);
  CHECK(ipsr(f) == armv8m::exc_hardfault);
  auto const state = saved(f);
  CHECK(armv8m::test_bit(state.exceptions.active, armv8m::exc_hardfault));
  CHECK(state.exceptions.nonsecure_active == 0);
  CHECK((f.word(hfsr) >> 30 & 1u) == 1);
  CHECK((f.word(cfsr) >> 16 & 1u) == 1);
  CHECK_FALSE(f.module->would_preempt(armv8m::exc_nmi));
}

TEST_CASE("a Non-secure UsageFault escalates to HardFault_NS with BFHFNMINS=1, B3.11 RDXYK")
{
  auto f = booted();
  poke(f, aircr, bfhfnmins);
  open_nonsecure_region(f);
  tables(f, armv8m::exc_hardfault);
  poke(f, vtor, ns_table);
  put(f, ns_code, {udf});
  REQUIRE(run_nonsecure(f, ns_code, 3).reason == stop_reason::user);
  CHECK(ipsr(f) == armv8m::exc_hardfault);
  auto const state = saved(f);
  CHECK(state.exceptions.nonsecure_active == 1u << armv8m::exc_hardfault);
  CHECK_FALSE(armv8m::test_bit(state.exceptions.active, armv8m::exc_hardfault));
  CHECK((f.word(cfsr + scs_ns) >> 16 & 1u) == 1);
  CHECK((f.word(cfsr) >> 16 & 1u) == 0);
  CHECK((f.word(hfsr + scs_ns) >> 30 & 1u) == 1);
  CHECK(f.module->would_preempt(armv8m::exc_nmi));
}

TEST_CASE("a Secure BusFault with BFHFNMINS=1 targets Non-secure and escalates to HardFault_NS")
{
  auto f = booted();
  poke(f, aircr, bfhfnmins);
  open_nonsecure_region(f);
  tables(f, armv8m::exc_hardfault);
  put(f, secure_code, {ldr_r1_r0, bkpt});
  f.set("r0", unmapped);
  REQUIRE(run_secure(f, secure_code, 3).reason == stop_reason::user);
  CHECK(ipsr(f) == armv8m::exc_hardfault);
  auto const state = saved(f);
  CHECK(state.exceptions.nonsecure_active == 1u << armv8m::exc_hardfault);
  CHECK_FALSE(armv8m::test_bit(state.exceptions.active, armv8m::exc_hardfault));
  CHECK((f.word(cfsr) >> 9 & 1u) == 1);
  CHECK((f.word(hfsr) >> 30 & 1u) == 1);
}

TEST_CASE("a Non-secure PendSV returns to the Non-secure bank, EXC_RETURN.ES from bit 0, B3.22")
{
  auto f = booted();
  open_nonsecure_region(f);
  tables(f, armv8m::exc_pendsv);
  poke(f, vtor, ns_table);
  put(f, ns_handler, {bx_lr});
  put(f, ns_code, {bkpt});
  poke(f, icsr + scs_ns, 1u << 28);
  REQUIRE(saved(f).exceptions.nonsecure_pending == 1u << armv8m::exc_pendsv);
  REQUIRE(run_nonsecure(f, ns_code, 3).reason == stop_reason::user);
  CHECK(ipsr(f) == 0);
  CHECK(f.reg("pc") == ns_code + 2);
  auto const state = saved(f);
  CHECK(state.exceptions.nonsecure_active == 0);
  CHECK(state.exceptions.nonsecure_pending == 0);
}

TEST_CASE("the handler runs in the state each exception targets, B3.11 RJQMQ, from either state")
{
  struct row
  {
    char const*   name;
    std::uint32_t number;
    address       pend_at;
    std::uint32_t bit;
    bool          bfhfnmins;
    bool          nonsecure_target;
  };
  for(auto const& one : {
          row{"NMI",           armv8m::exc_nmi,        icsr,          1u << 31, false, false},
          row{"NMI BFHFNMINS", armv8m::exc_nmi,        icsr,          1u << 31, true,  true },
          row{"BusFault",      armv8m::exc_busfault,   0xE000'ED24,   1u << 14, false, false},
          row{"BusFault NS",   armv8m::exc_busfault,   0xE000'ED24,   1u << 14, true,  true },
          row{"SVCall_S",      armv8m::exc_svcall,     0xE000'ED24,   1u << 15, false, false},
          row{"SVCall_NS",     armv8m::exc_svcall,     0xE002'ED24,   1u << 15, false, true },
          row{"UsageFault_NS", armv8m::exc_usagefault, 0xE002'ED24,   1u << 12, false, true },
          row{"MemManage_S",   armv8m::exc_memmanage,  0xE000'ED24,   1u << 13, false, false},
          row{"PendSV_NS",     armv8m::exc_pendsv,     icsr + scs_ns, 1u << 28, false, true },
          row{"SysTick_S",     armv8m::exc_systick,    icsr,          1u << 26, false, false},
          row{"IRQ ITNS 0",    16,                     ispr0,         1u << 0,  false, false},
          row{"IRQ ITNS 1",    16,                     ispr0,         1u << 0,  false, true },
  })
  {
    for(auto const from_secure : {true, false})
    {
      CAPTURE(one.name);
      CAPTURE(from_secure);
      auto f = booted();
      if(one.bfhfnmins)
      {
        poke(f, aircr, bfhfnmins);
      }
      open_nonsecure_region(f);
      tables(f, one.number);
      poke(f, iser0, 1);
      if(one.number == 16 and one.nonsecure_target)
      {
        poke(f, itns0, 1);
      }
      put(f, secure_code, {bkpt});
      put(f, ns_code, {bkpt});
      poke(f, one.pend_at, one.bit);
      auto const stopped =
          from_secure ? run_secure(f, secure_code, 2) : run_nonsecure(f, ns_code, 2);
      REQUIRE(stopped.reason == stop_reason::user);
      CHECK(ipsr(f) == one.number);
      CHECK(secure_now(f) == not one.nonsecure_target);
      CHECK(f.reg("pc") == (one.nonsecure_target ? ns_handler : secure_handler) + 2);
    }
  }
}

TEST_CASE(
    "a Secure thread preempted by a Non-secure IRQ stacks the additional state context, B3.19 "
    "B3.20 PushCalleeStack")
{
  for(auto const fp : {false, true})
  {
    CAPTURE(fp);
    auto f = booted();
    open_nonsecure_region(f);
    tables(f, 16);
    put(f, ns_handler, {bkpt, bx_lr});
    put(f, secure_code, {bkpt});
    poke(f, itns0, 1);
    poke(f, iser0, 1);
    if(fp)
    {
      engine_write(f, UC_ARM_REG_CONTROL_S, engine_read(f, UC_ARM_REG_CONTROL_S) | control_fpca);
    }
    set_registers(f);
    auto const msp  = engine_read(f, UC_ARM_REG_MSP_S);
    auto const size = fp ? 0x68u : 0x20u;
    REQUIRE((msp & 7u) == 0);
    auto const frame_at  = msp - size;
    auto const callee_at = frame_at - 0x28;
    poke(f, ispr0, 1);
    REQUIRE(run_secure(f, secure_code, 2).reason == stop_reason::user);
    CHECK_FALSE(secure_now(f));
    CHECK(ipsr(f) == 16);
    CHECK(f.reg("lr") == (fp ? 0xFFFF'FFE8u : 0xFFFF'FFF8u));
    CHECK(engine_read(f, UC_ARM_REG_MSP_S) == callee_at);
    auto const callee = popcorn::testing::words_at(*f.target, callee_at, 10);
    CHECK(callee[0] == (fp ? 0xFEFA'125Au : 0xFEFA'125Bu));
    for(std::uint32_t n = 4; n <= 11; ++n)
    {
      CHECK(callee[n - 2] == 0x100u + n);
    }
    auto const frame = popcorn::testing::words_at(*f.target, frame_at, 8);
    CHECK(frame[0] == 0x100);
    CHECK(frame[4] == 0x10c);
    CHECK(frame[6] == secure_code);
    for(int n = 0; n <= 12; ++n)
    {
      CAPTURE(n);
      CHECK(f.reg(std::format("r{}", n)) == 0);
    }
    REQUIRE(resume(f, 2).reason == stop_reason::user);
    CHECK(secure_now(f));
    CHECK(ipsr(f) == 0);
    CHECK(f.reg("pc") == secure_code + 2);
    CHECK(engine_read(f, UC_ARM_REG_MSP_S) == msp);
    CHECK(((engine_read(f, UC_ARM_REG_CONTROL_S) & control_fpca) != 0) == fp);
    for(int n = 0; n <= 12; ++n)
    {
      CAPTURE(n);
      CHECK(f.reg(std::format("r{}", n)) == 0x100u + static_cast<std::uint32_t>(n));
    }
  }
}

TEST_CASE("a corrupt integrity signature on return to Secure is a SecureFault INVIS, B3.22")
{
  auto f = booted();
  open_nonsecure_region(f);
  tables(f, 16);
  put(f, ns_handler, {bkpt, bx_lr});
  put(f, secure_code, {bkpt});
  poke(f, itns0, 1);
  poke(f, iser0, 1);
  poke(f, ispr0, 1);
  REQUIRE(run_secure(f, secure_code, 2).reason == stop_reason::user);
  auto const callee_at = engine_read(f, UC_ARM_REG_MSP_S);
  poke(f, callee_at, 0xFEFA'125Au);
  auto const stopped = resume(f, 2);
  REQUIRE(stopped.reason == stop_reason::fault);
  CHECK(stopped.fault->kind == popcorn::fault_kind::secure);
  CHECK(f.word(sfsr) == 1u << 1);
  CHECK_FALSE(secure_now(f));
}

TEST_CASE("a Non-secure handler returning with EXC_RETURN.ES set is a SecureFault INVER, B3.22")
{
  auto f = booted();
  open_nonsecure_region(f);
  tables(f, 16);
  put(f, ns_handler, {bkpt, bx_lr});
  put(f, ns_code, {bkpt});
  poke(f, itns0, 1);
  poke(f, iser0, 1);
  poke(f, ispr0, 1);
  REQUIRE(run_nonsecure(f, ns_code, 2).reason == stop_reason::user);
  REQUIRE(f.reg("lr") == 0xFFFF'FFB8u);
  f.set("lr", 0xFFFF'FFB9u);
  auto const stopped = resume(f, 2);
  REQUIRE(stopped.reason == stop_reason::fault);
  CHECK(stopped.fault->kind == popcorn::fault_kind::secure);
  CHECK(f.word(sfsr) == 1u << 2);
}

TEST_CASE("a Non-secure thread on PSP_NS preempted by a Secure IRQ, B3.19 and VTOR_S")
{
  auto f = booted();
  open_nonsecure_region(f);
  tables(f, 17);
  put(f, secure_handler, {bkpt, bx_lr});
  put(f, ns_code, {nop, bkpt});
  poke(f, iser0, 1u << 1);
  constexpr address psp = ns_stack - 0x100;
  engine_write(f, UC_ARM_REG_PSP_NS, psp);
  engine_write(f, UC_ARM_REG_CONTROL_NS, control_spsel);
  set_registers(f);
  poke(f, ispr0, 1u << 1);
  REQUIRE(run_nonsecure(f, ns_code, 2).reason == stop_reason::user);
  CHECK(secure_now(f));
  CHECK(ipsr(f) == 17);
  CHECK(f.reg("lr") == 0xFFFF'FFB9u);
  CHECK(engine_read(f, UC_ARM_REG_PSP_NS) == psp - 0x20);
  auto const frame = popcorn::testing::words_at(*f.target, psp - 0x20, 8);
  CHECK(frame[0] == 0x100);
  CHECK(frame[6] == ns_code);
  CHECK(f.reg("r0") == 0x100);
  REQUIRE(resume(f, 4).reason == stop_reason::user);
  CHECK_FALSE(secure_now(f));
  CHECK(ipsr(f) == 0);
  CHECK(f.reg("pc") == ns_code + 4);
  CHECK(engine_read(f, UC_ARM_REG_PSP_NS) == psp);
  CHECK((engine_read(f, UC_ARM_REG_CONTROL_NS) & control_spsel) != 0);
}

TEST_CASE("a Non-secure stack in Secure memory is a SecureFault AUVIOL before any store, B3.19")
{
  constexpr address secure_stack = secure_code + 0x1100;
  auto              f            = booted();
  open_nonsecure_region(f);
  tables(f, 16);
  put(f, ns_code, {bkpt});
  for(address at = secure_stack - 0x40; at < secure_stack; at += 4)
  {
    poke(f, at, fill);
  }
  poke(f, itns0, 1);
  poke(f, iser0, 1);
  poke(f, ispr0, 1);
  engine_write(f, UC_ARM_REG_SECURE, 0);
  engine_write(f, UC_ARM_REG_MSP_NS, secure_stack);
  f.set("pc", ns_code | 1u);
  auto const stopped = resume(f, 2);
  REQUIRE(stopped.reason == stop_reason::fault);
  CHECK(stopped.fault->kind == popcorn::fault_kind::secure);
  CHECK(f.word(sfsr) == ((1u << 3) | (1u << 6)));
  CHECK(f.word(sfar) == secure_stack - 0x20);
  for(address at = secure_stack - 0x40; at < secure_stack; at += 4)
  {
    CHECK(f.word(at) == fill);
  }
}

TEST_CASE("MSPLIM_NS above the Non-secure frame raises a Non-secure UsageFault STKOF, B3.21")
{
  auto f = booted();
  open_nonsecure_region(f);
  tables(f, 16);
  put(f, ns_code, {bkpt});
  poke(f, itns0, 1);
  poke(f, iser0, 1);
  poke(f, ispr0, 1);
  engine_write(f, UC_ARM_REG_MSPLIM_NS, ns_stack - 0x10);
  auto const stopped = run_nonsecure(f, ns_code, 2);
  REQUIRE(stopped.reason == stop_reason::fault);
  CHECK(stopped.fault->kind == popcorn::fault_kind::usage);
  CHECK((f.word(cfsr + scs_ns) >> 20 & 1u) == 1);
  CHECK((f.word(cfsr) >> 20 & 1u) == 0);
}

TEST_CASE(
    "a Secure handler returning with ES 0 is INVPC in UFSR_S, ValidateExceptionReturn lines "
    "42-60")
{
  auto f = booted(fault_policy::stop);
  tables(f, 16);
  put(f, secure_handler, {bkpt, bx_lr});
  put(f, secure_code, {bkpt});
  poke(f, iser0, 1);
  poke(f, ispr0, 1);
  REQUIRE(run_secure(f, secure_code, 2).reason == stop_reason::user);
  REQUIRE(ipsr(f) == 16);
  f.set("lr", 0xFFFF'FFBCu);
  auto const stopped = resume(f, 2);
  REQUIRE(stopped.reason == stop_reason::fault);
  CHECK(stopped.fault->kind == popcorn::fault_kind::usage);
  CHECK((f.word(cfsr) >> 18 & 1u) == 1);
  CHECK((f.word(cfsr + scs_ns) >> 18 & 1u) == 0);
}

TEST_CASE(
    "a forged stacked IPSR on return to a Secure thread is INVPC in UFSR_S, PopStack lines "
    "64-71")
{
  auto f = booted(fault_policy::stop);
  open_nonsecure_region(f);
  tables(f, 16);
  put(f, ns_handler, {bkpt, bx_lr});
  put(f, secure_code, {bkpt});
  poke(f, itns0, 1);
  poke(f, iser0, 1);
  poke(f, ispr0, 1);
  REQUIRE(run_secure(f, secure_code, 2).reason == stop_reason::user);
  REQUIRE_FALSE(secure_now(f));
  auto const frame_at = engine_read(f, UC_ARM_REG_MSP_S) + 0x28;
  poke(f, frame_at + 0x1c, f.word(frame_at + 0x1c) | 3u);
  auto const stopped = resume(f, 2);
  REQUIRE(stopped.reason == stop_reason::fault);
  CHECK(stopped.fault->kind == popcorn::fault_kind::usage);
  CHECK((f.word(cfsr) >> 18 & 1u) == 1);
  CHECK((f.word(cfsr + scs_ns) >> 18 & 1u) == 0);
}

TEST_CASE(
    "a Non-secure vector slot in Secure memory stops with HFSR.VECTTBL before the read, "
    "ValidateAddress lines 63-69")
{
  auto f = booted(fault_policy::stop);
  tables(f, 16);
  poke(f, vtor + scs_ns, secure_table);
  put(f, secure_code, {bkpt});
  poke(f, itns0, 1);
  poke(f, iser0, 1);
  poke(f, ispr0, 1);
  auto const stopped = run_secure(f, secure_code, 2);
  REQUIRE(stopped.reason == stop_reason::fault);
  CHECK(stopped.fault->kind == popcorn::fault_kind::hard);
  CHECK(stopped.fault->fault_address == secure_table + 4 * 16);
  CHECK((f.word(hfsr) >> 1 & 1u) == 1);
  CHECK(secure_now(f));
  CHECK(ipsr(f) == 0);
}

TEST_CASE(
    "a Non-secure unaligned LDREX is a Non-secure UsageFault UNALIGNED, or HardFault_S FORCED "
    "without USGFAULTENA_NS, B3.11 RJQMQ and B3.27")
{
  for(auto const enabled : {true, false})
  {
    CAPTURE(enabled);
    auto f = booted();
    open_nonsecure_region(f);
    tables(f, armv8m::exc_usagefault);
    tables(f, armv8m::exc_hardfault);
    if(enabled)
    {
      poke(f, 0xE000'ED24 + scs_ns, 1u << 18);
    }
    put(f, ns_code, {0xe850, 0x1f00, bkpt});
    f.set("r0", ns_base + 0x601);
    REQUIRE(run_nonsecure(f, ns_code, 3).reason == stop_reason::user);
    CHECK(ipsr(f) == (enabled ? armv8m::exc_usagefault : armv8m::exc_hardfault));
    CHECK(secure_now(f) != enabled);
    CHECK(f.reg("pc") == (enabled ? ns_handler : secure_handler) + 2);
    CHECK((f.word(cfsr + scs_ns) >> 24 & 1u) == 1);
    CHECK((f.word(cfsr) >> 24 & 1u) == 0);
    CHECK((f.word(hfsr) >> 30 & 1u) == (enabled ? 0u : 1u));
  }
}

TEST_CASE("the NMI line runs the Non-secure NMI with BFHFNMINS=1 and pends again on return")
{
  auto f = booted();
  poke(f, aircr, bfhfnmins);
  open_nonsecure_region(f);
  tables(f, armv8m::exc_nmi);
  put(f, ns_handler, {bkpt, bx_lr});
  put(f, secure_code, {bkpt});
  f.module->set_nmi_line(true);
  REQUIRE(run_secure(f, secure_code, 2).reason == stop_reason::user);
  CHECK(ipsr(f) == armv8m::exc_nmi);
  CHECK_FALSE(secure_now(f));
  REQUIRE(resume(f, 2).reason == stop_reason::user);
  CHECK(ipsr(f) == armv8m::exc_nmi);
  CHECK_FALSE(secure_now(f));
  CHECK(f.reg("pc") == ns_handler + 2);
  f.module->set_nmi_line(false);
  REQUIRE(resume(f, 2).reason == stop_reason::user);
  CHECK(ipsr(f) == 0);
  CHECK(secure_now(f));
  CHECK(f.reg("pc") == secure_code + 2);
}

TEST_CASE("sleep-on-exit reads the SCR of the state returned to, B3.22 ExceptionReturn line 66")
{
  for(auto const nonsecure_scr : {true, false})
  {
    CAPTURE(nonsecure_scr);
    auto f = booted();
    open_nonsecure_region(f);
    tables(f, 16);
    put(f, ns_handler, {bx_lr});
    put(f, ns_code, {bkpt});
    poke(f, itns0, 1);
    poke(f, iser0, 1);
    poke(f, nonsecure_scr ? scr + scs_ns : scr, 1u << 1);
    poke(f, ispr0, 1);
    engine_write(f, UC_ARM_REG_MSP_NS, ns_stack);
    engine_write(f, UC_ARM_REG_SECURE, 0);
    f.set("pc", ns_code | 1u);
    std::ignore = f.run({.max_instructions = icount{4}});
    CHECK(f.module->idle() == nonsecure_scr);
  }
}

namespace
{
constexpr std::uint32_t fpccr_ts        = 1u << 26;
constexpr std::uint32_t fpccr_s         = 1u << 2;
constexpr std::uint32_t fpccr_lspact    = 1u << 0;
constexpr std::uint32_t cpacr_cp10_cp11 = 0x00F0'0000;
constexpr std::uint32_t sfsr_lserr      = 1u << 7;
constexpr std::uint32_t ufsr_nocp       = 1u << 19;

auto pattern(int n) -> std::uint32_t
{
  return 0x4100'0000u + static_cast<std::uint32_t>(n);
}

void load_s_registers(arch_fixture const& f)
{
  for(int n = 0; n < 32; ++n)
  {
    f.set(std::format("s{}", n), pattern(n));
  }
}

auto vmov_s_r0(int n) -> std::array<std::uint16_t, 2>
{
  return {
      static_cast<std::uint16_t>(0xee00u | static_cast<unsigned>(n >> 1)),
      static_cast<std::uint16_t>(0x0a10u | (static_cast<unsigned>(n & 1) << 7))};
}

void put_clobber(arch_fixture const& f, address at)
{
  for(int n = 0; n < 32; ++n)
  {
    auto const insn = vmov_s_r0(n);
    put(f, at + 4 * static_cast<address>(n), {insn[0], insn[1]});
  }
  put(f, at + 0x80, {bkpt, bx_lr});
}

constexpr std::uint16_t vmov_s0_r2  = 0xee00;
constexpr std::uint16_t vmov_s0_r2b = 0x2a10;
constexpr std::uint16_t sub_sp_0x88 = 0xb0a2;
constexpr std::uint16_t vlstm_sp    = 0xec2d;
constexpr std::uint16_t vlxxm_tail  = 0x0a00;
constexpr std::uint16_t blxns_r3    = 0x479c;
constexpr std::uint16_t vlldm_sp    = 0xec3d;

void fill_below(arch_fixture const& f, address top, std::uint32_t bytes)
{
  for(address at = top - bytes; at < top; at += 4)
  {
    poke(f, at, fill);
  }
}
}

TEST_CASE(
    "SHCSR_S.SECUREFAULTENA and SECUREFAULTPENDED are Secure-only under TrustZone, D1.2 SHCSR")
{
  auto f = booted();
  poke(f, shcsr, (1u << 19) | (1u << 20));
  CHECK((f.word(shcsr) & ((1u << 19) | (1u << 20))) == ((1u << 19) | (1u << 20)));
  CHECK((f.word(shcsr + scs_ns) & ((1u << 19) | (1u << 20))) == 0);
  CHECK(saved(f).scs.shcsr_enables == 1u << 19);
  CHECK(armv8m::test_bit(saved(f).exceptions.pending, armv8m::exc_securefault));
  poke(f, shcsr, 0);
  CHECK_FALSE(armv8m::test_bit(saved(f).exceptions.pending, armv8m::exc_securefault));
  arch_fixture plain{popcorn::testing::m33(), "scs", core::machine_options{}};
  REQUIRE(plain.run_to_stop().reason == stop_reason::user);
  poke(plain, shcsr, (1u << 19) | (1u << 20));
  CHECK((plain.word(shcsr) & ((1u << 19) | (1u << 20))) == 0);
}

TEST_CASE(
    "a VLSTM with LSPACT set in the bank FPCCR_S.S selects is a SecureFault LSERR, C2.4.368 "
    "lines 12-16, escalated to HardFault_S without SECUREFAULTENA, B3.12")
{
  for(auto const policy : {fault_policy::stop, fault_policy::enter_handler})
  {
    for(auto const enabled : {false, true})
    {
      CAPTURE(policy == fault_policy::stop);
      CAPTURE(enabled);
      auto f = booted(policy);
      poke(f, aircr, bfhfnmins);
      poke(f, cpacr, cpacr_cp10_cp11);
      tables(f, armv8m::exc_hardfault);
      tables(f, armv8m::exc_securefault);
      if(enabled)
      {
        poke(f, shcsr, 1u << 19);
      }
      put(f, secure_code, {vmov_s0_r2, vmov_s0_r2b, bkpt});
      REQUIRE(run_secure(f, secure_code, 2).reason == stop_reason::user);
      engine_write(
          f, UC_ARM_REG_FPCCR_S, engine_read(f, UC_ARM_REG_FPCCR_S) | fpccr_s | fpccr_lspact);
      constexpr address after = secure_code + 0x10;
      put(f, after, {vlstm_sp, vlxxm_tail, bkpt});
      auto const stopped = run_secure(f, after, 2);
      CHECK((f.word(sfsr) & sfsr_lserr) != 0);
      if(policy == fault_policy::stop)
      {
        REQUIRE(stopped.reason == stop_reason::fault);
        CHECK(stopped.fault->kind == popcorn::fault_kind::secure);
        CHECK((stopped.fault->status & sfsr_lserr) != 0);
        continue;
      }
      REQUIRE(stopped.reason == stop_reason::user);
      CHECK(secure_now(f));
      CHECK(f.reg("pc") == secure_handler + 2);
      CHECK(ipsr(f) == (enabled ? armv8m::exc_securefault : armv8m::exc_hardfault));
      CHECK((f.word(hfsr) >> 30 & 1u) == (enabled ? 0u : 1u));
      CHECK(f.module->would_preempt(armv8m::exc_nmi) == enabled);
    }
  }
}

TEST_CASE(
    "VLSTM before a BLXNS saves S0-S15 and FPSCR, S16-S31 with FPCCR_S.TS, and clears them, "
    "VLLDM after FNC_RETURN restores them, C2.4.368, C2.4.367 and B3.19")
{
  for(auto const ts : {false, true})
  {
    CAPTURE(ts);
    auto f = booted();
    open_nonsecure_region(f);
    poke(f, cpacr, cpacr_cp10_cp11);
    if(ts)
    {
      poke(f, fpccr, f.word(fpccr) | fpccr_ts);
    }
    auto const sp   = engine_read(f, UC_ARM_REG_MSP_S);
    auto const area = sp - 0x88;
    fill_below(f, sp, 0x88);
    put_clobber(f, ns_code);
    put(f, secure_code,
        {vmov_s0_r2, vmov_s0_r2b, sub_sp_0x88, vlstm_sp, vlxxm_tail, bkpt, blxns_r3, vlldm_sp,
         vlxxm_tail, bkpt});
    load_s_registers(f);
    f.set("r2", pattern(0));
    f.set("r3", ns_code);
    f.set("r0", 0xDEAD'0000);
    engine_write(f, UC_ARM_REG_MSP_NS, ns_stack);
    REQUIRE(run_secure(f, secure_code, 4).reason == stop_reason::user);
    for(int n = 0; n < 32; ++n)
    {
      CAPTURE(n);
      CHECK(f.reg(std::format("s{}", n)) == (ts ? 0u : pattern(n)));
    }
    REQUIRE(resume(f, 40).reason == stop_reason::user);
    REQUIRE(f.reg("pc") == ns_code + 0x82);
    CHECK_FALSE(secure_now(f));
    auto const saved_area = popcorn::testing::words_at(*f.target, area, 0x88 / 4);
    for(int n = 0; n < 16; ++n)
    {
      CAPTURE(n);
      CHECK(saved_area[static_cast<std::size_t>(n)] == pattern(n));
    }
    for(int n = 0; n < 16; ++n)
    {
      CAPTURE(n);
      CHECK(saved_area[static_cast<std::size_t>(0x12 + n)] == (ts ? pattern(16 + n) : fill));
    }
    REQUIRE(resume(f, 6).reason == stop_reason::user);
    CHECK(secure_now(f));
    CHECK(f.reg("pc") == secure_code + 20);
    for(int n = 0; n < 32; ++n)
    {
      CAPTURE(n);
      CHECK(f.reg(std::format("s{}", n)) == (n < 16 or ts ? pattern(n) : 0xDEAD'0000u));
    }
  }
}

TEST_CASE(
    "a hard-float cmse_nonsecure_call saves d8-d15 with VPUSH on the Secure stack only and "
    "restores them with VPOP after FNC_RETURN, B3.19 and C2.4.368")
{
  auto f = booted(fault_policy::enter_handler, "cmse");
  open_nonsecure_region(f);
  poke(f, cpacr, cpacr_cp10_cp11);
  put_clobber(f, ns_code);
  fill_below(f, ns_stack, 0x100);
  engine_write(f, UC_ARM_REG_MSP_NS, ns_stack);
  poke(f, popcorn::testing::sym("cmse", "cmse_callee"), ns_code | 1u);
  load_s_registers(f);
  f.set("r0", 0xDEAD'0000);
  REQUIRE(
      run_secure(f, popcorn::testing::sym("cmse", "cmse_call") & ~1u, 80).reason
      == stop_reason::user);
  REQUIRE(f.reg("pc") == ns_code + 0x82);
  CHECK_FALSE(secure_now(f));
  auto const        msp_s   = armv8m::read_bank(engine_of(f), security::secure).msp;
  constexpr address ram_end = 0x200C'0000;
  REQUIRE(msp_s < ram_end);
  auto const stack = popcorn::testing::words_at(
      *f.target, msp_s, std::min<std::size_t>(0x40, (ram_end - msp_s) / 4));
  auto const found = std::ranges::search(
      stack, std::views::iota(16, 32) | std::views::transform([](int n) { return pattern(n); }));
  CHECK_FALSE(found.empty());
  auto const ns_words = popcorn::testing::words_at(*f.target, ns_stack - 0x100, 0x40);
  CHECK(std::ranges::all_of(ns_words, [](std::uint32_t word) { return word == fill; }));
  CHECK(f.reg("s16") == f.reg("r0"));
  REQUIRE(resume(f, 40).reason == stop_reason::user);
  CHECK(secure_now(f));
  CHECK(f.reg("pc") == (popcorn::testing::sym("cmse", "cmse_call") & ~1u) + 0x58);
  for(int n = 16; n < 32; ++n)
  {
    CAPTURE(n);
    CHECK(f.reg(std::format("s{}", n)) == pattern(n));
  }
}

TEST_CASE(
    "a VLSTM with CPACR_S CP10 0 is a Secure UsageFault NOCP, frame untouched, the Secure FP "
    "instruction before it not faulting, a deviation from C2.4.368 line 6 and B3.36")
{
  auto       f  = booted(fault_policy::stop);
  auto const sp = engine_read(f, UC_ARM_REG_MSP_S);
  fill_below(f, sp, 0x88);
  put(f, secure_code, {vmov_s0_r2, vmov_s0_r2b, sub_sp_0x88, vlstm_sp, vlxxm_tail, bkpt});
  auto const stopped = run_secure(f, secure_code, 4);
  REQUIRE(stopped.reason == stop_reason::fault);
  CHECK(stopped.fault->kind == popcorn::fault_kind::usage);
  CHECK(f.reg("pc") == secure_code + 6);
  CHECK((f.word(cfsr) & ufsr_nocp) != 0);
  CHECK((f.word(cfsr + scs_ns) & ufsr_nocp) == 0);
  auto const area = popcorn::testing::words_at(*f.target, sp - 0x88, 0x88 / 4);
  CHECK(std::ranges::all_of(area, [](std::uint32_t word) { return word == fill; }));
}

TEST_CASE(
    "a Secure FP background with FPCCR_S.TS stacks S16-S31 after the extended frame and clears "
    "the FP registers, PushStack lines 9-80")
{
  auto f = booted();
  open_nonsecure_region(f);
  tables(f, 16);
  put(f, ns_handler, {bkpt, bx_lr});
  put(f, secure_code, {bkpt});
  poke(f, itns0, 1);
  poke(f, iser0, 1);
  poke(f, fpccr, f.word(fpccr) | fpccr_ts);
  engine_write(f, UC_ARM_REG_CONTROL_S, engine_read(f, UC_ARM_REG_CONTROL_S) | control_fpca);
  load_s_registers(f);
  auto const msp      = engine_read(f, UC_ARM_REG_MSP_S);
  auto const frame_at = msp - 0xA8;
  poke(f, ispr0, 1);
  REQUIRE(run_secure(f, secure_code, 2).reason == stop_reason::user);
  CHECK(engine_read(f, UC_ARM_REG_MSP_S) == frame_at - 0x28);
  auto const frame = popcorn::testing::words_at(*f.target, frame_at, 0xA8 / 4);
  for(int n = 0; n < 16; ++n)
  {
    CAPTURE(n);
    CHECK(frame[static_cast<std::size_t>(8 + n)] == pattern(n));
    CHECK(frame[static_cast<std::size_t>(26 + n)] == pattern(16 + n));
  }
  for(int n = 0; n < 32; ++n)
  {
    CAPTURE(n);
    CHECK(f.reg(std::format("s{}", n)) == 0);
  }
  REQUIRE(resume(f, 2).reason == stop_reason::user);
  CHECK(f.reg("pc") == secure_code + 2);
  CHECK(engine_read(f, UC_ARM_REG_MSP_S) == msp);
  for(int n = 0; n < 32; ++n)
  {
    CAPTURE(n);
    CHECK(f.reg(std::format("s{}", n)) == pattern(n));
  }
}

TEST_CASE(
    "a Secure IRQ preempting a Non-secure handler records CONTROL_S.SPSEL in EXC_RETURN with "
    "Mode 0 and returns, ExceptionTaken lines 30-35")
{
  auto f = booted();
  open_nonsecure_region(f);
  tables(f, 16);
  poke(f, secure_table + 4 * 17, secure_handler | 1u);
  put(f, ns_handler, {bkpt, bx_lr});
  put(f, secure_handler, {bkpt, bx_lr});
  put(f, ns_code, {bkpt});
  poke(f, itns0, 1);
  poke(f, iser0, 3);
  poke(f, 0xE000'E400, 0x0000'0040);
  engine_write(f, UC_ARM_REG_CONTROL_S, engine_read(f, UC_ARM_REG_CONTROL_S) | control_spsel);
  poke(f, ispr0, 1);
  REQUIRE(run_nonsecure(f, ns_code, 2).reason == stop_reason::user);
  REQUIRE(ipsr(f) == 16);
  poke(f, ispr0, 2);
  REQUIRE(resume(f, 2).reason == stop_reason::user);
  CHECK(ipsr(f) == 17);
  CHECK(secure_now(f));
  CHECK(f.reg("lr") == 0xFFFF'FFB5u);
  REQUIRE(resume(f, 4).reason == stop_reason::user);
  CHECK(ipsr(f) == 0);
  CHECK_FALSE(secure_now(f));
  CHECK(f.reg("pc") == ns_code + 2);
  CHECK((engine_read(f, UC_ARM_REG_CONTROL_S) & control_spsel) != 0);
}

TEST_CASE(
    "a Non-secure IRQ over Non-secure code with FPCA set and NSACR.CP10 0 stops with UFSR_S.NOCP "
    "before any store, PushStack lines 15 and 52-54")
{
  auto f = booted(fault_policy::stop);
  open_nonsecure_region(f);
  tables(f, 16);
  put(f, secure_code, {vmov_s0_r2, vmov_s0_r2b, blxns_r3, bkpt});
  put(f, ns_code, {bkpt, 0xe7fe});
  fill_below(f, ns_stack, 0x80);
  engine_write(f, UC_ARM_REG_MSP_NS, ns_stack);
  poke(f, itns0, 1);
  poke(f, iser0, 1);
  f.set("r3", ns_code);
  REQUIRE(run_secure(f, secure_code, 4).reason == stop_reason::user);
  REQUIRE_FALSE(secure_now(f));
  REQUIRE((f.reg("control") & control_fpca) != 0);
  poke(f, ispr0, 1);
  auto const stopped = resume(f, 2);
  REQUIRE(stopped.reason == stop_reason::fault);
  CHECK(stopped.fault->kind == popcorn::fault_kind::usage);
  CHECK((f.word(cfsr) & ufsr_nocp) != 0);
  CHECK((f.word(cfsr + scs_ns) & ufsr_nocp) == 0);
  CHECK(engine_read(f, UC_ARM_REG_MSP_NS) == ns_stack);
  auto const stack = popcorn::testing::words_at(*f.target, ns_stack - 0x80, 0x20);
  CHECK(std::ranges::all_of(stack, [](std::uint32_t word) { return word == fill; }));
}

TEST_CASE("a SecureFault that cannot preempt stops as hard with CFSR as status, SFSR kept apart")
{
  auto f = booted();
  poke(f, cpacr, cpacr_cp10_cp11);
  poke(f, shcsr, 1u << 19);
  put(f, secure_code, {vmov_s0_r2, vmov_s0_r2b, bkpt});
  REQUIRE(run_secure(f, secure_code, 2).reason == stop_reason::user);
  engine_write(f, UC_ARM_REG_FPCCR_S, engine_read(f, UC_ARM_REG_FPCCR_S) | fpccr_s | fpccr_lspact);
  engine_write(f, UC_ARM_REG_FAULTMASK_S, 1);
  constexpr address after = secure_code + 0x10;
  put(f, after, {vlstm_sp, vlxxm_tail, bkpt});
  auto const stopped = run_secure(f, after, 2);
  REQUIRE(stopped.reason == stop_reason::fault);
  CHECK(stopped.fault->kind == popcorn::fault_kind::hard);
  CHECK(stopped.fault->status == f.word(cfsr));
  CHECK((stopped.fault->status & sfsr_lserr) == 0);
  CHECK((f.word(sfsr) & sfsr_lserr) != 0);
}
