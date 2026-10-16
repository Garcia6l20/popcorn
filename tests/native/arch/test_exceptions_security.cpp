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

#include <array>
#include <cstdint>
#include <initializer_list>
#include <random>
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
constexpr address icsr   = 0xE000'ED04;
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

auto booted(fault_policy policy = fault_policy::enter_handler) -> arch_fixture
{
  auto options   = trustzone_options();
  options.faults = policy;
  arch_fixture f{m33_trustzone(), "scs", options};
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
