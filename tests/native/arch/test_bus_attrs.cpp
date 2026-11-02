#include "arch_fixture.hpp"

#include "../../../native/src/core/machine_impl.hpp"
#include "../support/inert_model.hpp"
#include "../support/test_model.hpp"
#include "exceptions.hpp"
#include "state.hpp"

#include <popcorn/core/errors.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/periph/firewall.hpp>

#include <doctest/doctest.h>
#include <reflex/serde/bson.hpp>

#include <unicorn/unicorn.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <initializer_list>
#include <memory>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace doctest
{
template <> struct StringMaker<popcorn::bus_attrs>
{
  static auto convert(popcorn::bus_attrs const& attrs) -> String
  {
    return std::format("{{{}, secure {}, privileged {}}}", attrs.by, attrs.secure, attrs.privileged)
        .c_str();
  }
};
}

namespace
{
namespace core   = popcorn::core;
namespace periph = popcorn::periph;
using periph::bus_verdict;
using popcorn::address;
using popcorn::bus_attrs;
using popcorn::icount;
using popcorn::initiator;
using popcorn::stop_reason;
using popcorn::arch::memory_op;
using popcorn::testing::arch_fixture;
using popcorn::testing::m33_trustzone;
using popcorn::testing::test_model;
using popcorn::testing::trustzone_options;

constexpr address code_at     = 0x2008'0000;
constexpr address testdev     = 0x4001'0000;
constexpr address sec_testdev = 0x5001'0000;
constexpr address data        = test_model::data_offset;
constexpr address sau_ctrl    = 0xE000'EDD0;

constexpr std::uint32_t sau_allns = 1u << 1;
constexpr std::uint16_t store     = 0x6001;
constexpr std::uint16_t load      = 0x6801;
constexpr std::uint16_t svc       = 0xdf00;
constexpr std::uint16_t bkpt      = 0xbe01;
constexpr std::uint16_t bx_lr     = 0x4770;

constexpr std::uint32_t exc_return_thread_msp  = 0xFFFF'FFF9;
constexpr std::uint32_t exc_return_handler_msp = 0xFFFF'FFF1;

constexpr std::uint32_t cfsr_impreciserr = 1u << 10;
constexpr std::uint32_t cfsr_preciserr   = 1u << 9;
constexpr std::uint32_t cfsr_bfarvalid   = 1u << 15;
constexpr address       cfsr             = 0xE000'ED28;
constexpr address       bfar             = 0xE000'ED38;
constexpr address       hfsr             = 0xE000'ED2C;
constexpr address       shcsr            = 0xE000'ED24;
constexpr std::uint32_t hfsr_forced      = 1u << 30;

class probe_firewall final : public popcorn::testing::inert_model, public periph::bus_firewall
{
public:
  struct call
  {
    std::size_t unit{};
    bus_attrs   attrs{};
    memory_op   op{};
  };

  explicit probe_firewall(periph::model_context& ctx) : context{ctx}
  {
    ctx.install_firewall(*this);
  }

  auto check_peripheral(std::size_t unit, bus_attrs attrs, memory_op op) -> bus_verdict override
  {
    calls.push_back({.unit = unit, .attrs = attrs, .op = op});
    return verdict;
  }

  periph::model_context& context;
  std::vector<call>      calls{};
  bus_verdict            verdict = bus_verdict::allow;
};

struct idle_firewall final : periph::bus_firewall
{
  auto check_peripheral(std::size_t, bus_attrs, memory_op) -> bus_verdict override
  {
    return bus_verdict::allow;
  }
};

struct rig
{
  std::shared_ptr<test_model>     model{};
  std::shared_ptr<probe_firewall> firewall{};
  std::unique_ptr<arch_fixture>   f{};
};

auto unit_of(std::string_view name) -> std::size_t
{
  auto const desc  = m33_trustzone();
  auto const found = std::ranges::find(desc.peripherals, name, &popcorn::peripheral_desc::name);
  REQUIRE(found != desc.peripherals.end());
  return static_cast<std::size_t>(found - desc.peripherals.begin());
}

auto booted(
    bool                        trustzone,
    bool                        firewall,
    popcorn::arch::fault_policy faults = popcorn::arch::fault_policy::stop) -> rig
{
  rig  out;
  auto options   = trustzone ? trustzone_options() : core::machine_options{};
  options.faults = faults;
  options.models.emplace(
      "TESTDEV",
      [&out](periph::model_context& ctx, popcorn::peripheral_desc const&)
          -> std::shared_ptr<periph::model> {
        out.model = std::make_shared<test_model>(ctx);
        return out.model;
      });
  if(firewall)
  {
    options.models.emplace(
        "UNMODELLED",
        [&out](periph::model_context& ctx, popcorn::peripheral_desc const&)
            -> std::shared_ptr<periph::model> {
          out.firewall = std::make_shared<probe_firewall>(ctx);
          return out.firewall;
        });
  }
  out.f = std::make_unique<arch_fixture>(m33_trustzone(), "scs", std::move(options));
  REQUIRE(out.f->run_to_stop().reason == stop_reason::user);
  return out;
}

void engine_secure(rig const& r, bool secure)
{
  std::uint32_t value = secure ? 1 : 0;
  REQUIRE(
      uc_reg_write(core::machine_internals::engine(*r.f->target), UC_ARM_REG_SECURE, &value)
      == UC_ERR_OK);
}

struct cpu_access
{
  std::uint16_t insn{};
  address       at{};
  std::uint32_t value{};
  bool          nonsecure{};
  bool          unprivileged{};
  std::uint64_t budget{2};
};

auto run_cpu(rig const& r, cpu_access const& access) -> popcorn::stop
{
  if(access.nonsecure)
  {
    REQUIRE(r.f->target->mem().write(sau_ctrl, 4, sau_allns));
    engine_secure(r, false);
  }
  r.f->set("control", access.unprivileged ? 1 : 0);
  REQUIRE(r.f->target->mem().write(code_at, 2, access.insn));
  REQUIRE(r.f->target->mem().write(code_at + 2, 2, bkpt));
  r.f->set("r0", access.at);
  r.f->set("r1", access.value);
  r.f->set("pc", code_at | 1u);
  auto const stopped = r.f->run_to_stop({.max_instructions = icount{access.budget}});
  r.f->set("control", 0);
  if(access.nonsecure)
  {
    engine_secure(r, true);
    REQUIRE(r.f->target->mem().write(sau_ctrl, 4, 0));
  }
  return stopped;
}

auto last_ctx(rig const& r) -> periph::access_ctx
{
  REQUIRE_FALSE(r.model->accesses.empty());
  return r.model->accesses.back().ctx;
}

auto last_mmio(rig const& r) -> core::trace_record
{
  auto const records = r.f->target->trace().records();
  auto const found   = std::ranges::find(
      records | std::views::reverse, core::trace_kind::mmio, &core::trace_record::kind);
  REQUIRE(found != (records | std::views::reverse).end());
  return *found;
}
}

TEST_CASE("access_ctx::secure is the transaction attribute under TrustZone, P5 and option B")
{
  auto r = booted(true, false);
  struct row
  {
    cpu_access access;
    bool       secure;
    bool       alias;
    bool       privileged;
  };
  for(auto const& one : std::initializer_list<row>{
          {{.insn = store, .at = testdev + data, .value = 1},                       true,  false, true},
          {{.insn = store, .at = sec_testdev + data, .value = 2},                   true,  true,  true},
          {{.insn = load, .at = testdev + data},                                    true,  false, true},
          {{.insn = store, .at = testdev + data, .value = 3, .unprivileged = true},
           true,                                                                           false,
           false                                                                                      },
          {{.insn = store, .at = testdev + data, .value = 4, .nonsecure = true},
           false,                                                                          false,
           true                                                                                       },
          {{.insn = load, .at = testdev + data, .nonsecure = true},                 false, false, true},
  })
  {
    CAPTURE(one.access.at);
    CAPTURE(one.access.insn);
    CAPTURE(one.access.nonsecure);
    REQUIRE(run_cpu(r, one.access).reason == stop_reason::user);
    auto const ctx = last_ctx(r);
    CHECK(ctx.by == initiator::cpu);
    CHECK(ctx.secure == one.secure);
    CHECK(ctx.via_secure_alias == one.alias);
    CHECK(ctx.privileged == one.privileged);
  }
  REQUIRE(r.f->target->mem().write(testdev + data, 4, 5));
  CHECK(last_ctx(r).by == initiator::debugger);
  CHECK(last_ctx(r).secure);
  CHECK(last_ctx(r).privileged);
}

TEST_CASE("a Non-secure CPU store to a SEC_ window is refused before MMIO, B10.4 IDAU and SAU")
{
  auto       r      = booted(true, false);
  auto const before = r.model->accesses.size();
  auto const stopped =
      run_cpu(r, {.insn = store, .at = sec_testdev + data, .value = 7, .nonsecure = true});
  REQUIRE(stopped.reason == stop_reason::fault);
  REQUIRE(stopped.fault.has_value());
  CHECK(stopped.fault->fault_address == sec_testdev + data);
  CHECK(r.model->accesses.size() == before);
}

TEST_CASE(
    "without TrustZone access_ctx::secure is false for every access, via_secure_alias the window")
{
  auto r = booted(false, false);
  REQUIRE(
      run_cpu(r, {.insn = store, .at = testdev + data, .value = 1}).reason == stop_reason::user);
  CHECK_FALSE(last_ctx(r).secure);
  CHECK_FALSE(last_ctx(r).via_secure_alias);
  REQUIRE(
      run_cpu(r, {.insn = store, .at = sec_testdev + data, .value = 2}).reason
      == stop_reason::user);
  CHECK_FALSE(last_ctx(r).secure);
  CHECK(last_ctx(r).via_secure_alias);
  REQUIRE(r.f->target->mem().write(sec_testdev + data, 4, 3));
  CHECK(last_ctx(r).by == initiator::debugger);
  CHECK_FALSE(last_ctx(r).secure);
  CHECK(last_ctx(r).via_secure_alias);
  auto& services = r.model->context().services;
  REQUIRE(services.write(testdev + data, 4, 4, bus_attrs{.by = initiator::dma}));
  CHECK(last_ctx(r).by == initiator::dma);
  CHECK_FALSE(last_ctx(r).secure);
  CHECK_FALSE(r.model->context().trustzone);
}

TEST_CASE("the firewall is asked first, read_zero_write_ignore keeps the model out, RM0456 5.4.2")
{
  auto r = booted(false, true);
  REQUIRE(r.f->target->mem().write(testdev + data, 4, 0x1234));
  auto const before = r.model->accesses.size();
  r.firewall->calls.clear();
  r.firewall->verdict = bus_verdict::read_zero_write_ignore;

  REQUIRE(run_cpu(r, {.insn = load, .at = testdev + data}).reason == stop_reason::user);
  CHECK(r.f->reg("r1") == 0);
  CHECK(r.model->accesses.size() == before);
  CHECK(last_mmio(r).payload[1] == testdev + data);
  CHECK(last_mmio(r).payload[2] == 0);
  REQUIRE(r.firewall->calls.size() == 1);
  CHECK(r.firewall->calls[0].unit == unit_of("TESTDEV"));
  CHECK(r.firewall->calls[0].op == memory_op::read);
  CHECK(r.firewall->calls[0].attrs == bus_attrs{.by = initiator::cpu, .secure = false});

  REQUIRE(
      run_cpu(r, {.insn = store, .at = sec_testdev + data, .value = 0x55}).reason
      == stop_reason::user);
  CHECK(r.model->accesses.size() == before);
  CHECK(last_mmio(r).payload[1] == sec_testdev + data);
  CHECK(last_mmio(r).payload[2] == 0x55);
  REQUIRE(r.firewall->calls.size() == 2);
  CHECK(r.firewall->calls[1].unit == unit_of("TESTDEV"));
  CHECK(r.firewall->calls[1].op == memory_op::write);

  r.firewall->verdict = bus_verdict::allow;
  CHECK(r.f->word(testdev + data) == 0x1234);
  CHECK(r.model->accesses.size() == before + 1);
}

TEST_CASE("a firewall bus_error is a precise BusFault at the access, BFAR its address, B3.27")
{
  auto r = booted(false, true);
  REQUIRE(r.f->target->mem().write(testdev + data, 4, 0x1234));
  r.firewall->verdict = bus_verdict::bus_error;
  auto const before   = r.model->accesses.size();
  for(auto const insn : {load, store})
  {
    CAPTURE(insn);
    auto const stopped = run_cpu(r, {.insn = insn, .at = testdev + data, .value = 0x77});
    REQUIRE(stopped.reason == stop_reason::fault);
    REQUIRE(stopped.fault.has_value());
    CHECK(stopped.fault->kind == popcorn::fault_kind::bus);
    CHECK(stopped.fault->fault_address == testdev + data);
    CHECK(stopped.fault->status == (cfsr_preciserr | cfsr_bfarvalid));
    CHECK(r.f->word(bfar) == testdev + data);
    CHECK(r.f->reg("pc") == code_at);
    CHECK(r.f->reg("r1") == 0x77);
    CHECK(r.model->accesses.size() == before);
    REQUIRE(r.f->target->mem().write(cfsr, 4, 0xffff'ffff));
  }
  CHECK_FALSE(r.f->target->mem().read(testdev + data, 4).has_value());
  r.firewall->verdict = bus_verdict::allow;
  CHECK(r.f->word(testdev + data) == 0x1234);
}

TEST_CASE("under enter_handler a firewall bus_error stacks the pc of the faulting access")
{
  auto r              = booted(false, true, popcorn::arch::fault_policy::enter_handler);
  r.firewall->verdict = bus_verdict::bus_error;
  REQUIRE(r.f->target->mem().write(code_at, 2, load));
  REQUIRE(r.f->target->mem().write(code_at + 2, 2, bkpt));
  r.f->set("r0", testdev + data);
  r.f->set("pc", code_at | 1u);
  auto const msp = static_cast<address>(r.f->reg("msp"));
  std::ignore    = r.f->run({.max_instructions = icount{1}});
  CHECK(r.f->reg("pc") == code_at);
  std::ignore = r.f->run({.max_instructions = icount{1}});
  CHECK((r.f->reg("xpsr") & 0x1ff) == 3);
  CHECK((r.f->word(hfsr) & hfsr_forced) != 0);
  CHECK(r.f->reg("msp") <= msp - 0x20);
  CHECK(r.f->word(msp - 0x20 + 0x18) == code_at);
  CHECK((r.f->word(cfsr) & (cfsr_preciserr | cfsr_bfarvalid)) == (cfsr_preciserr | cfsr_bfarvalid));
  CHECK(r.f->word(bfar) == testdev + data);
}

TEST_CASE("a model bus error behind an allowing firewall stays imprecise, the control leg")
{
  auto r              = booted(false, true);
  r.model->fail_reads = true;
  auto const stopped  = run_cpu(r, {.insn = load, .at = testdev + data});
  REQUIRE(stopped.reason == stop_reason::fault);
  REQUIRE(stopped.fault.has_value());
  CHECK(stopped.fault->kind == popcorn::fault_kind::bus);
  CHECK(stopped.fault->fault_address == testdev + data);
  CHECK(stopped.fault->status == cfsr_impreciserr);
}

TEST_CASE(
    "a refused host CPU access, stacking or a read, leaves no refusal for the next model bus error")
{
  auto r              = booted(false, true);
  r.firewall->verdict = bus_verdict::bus_error;
  r.f->set("msp", testdev + 0x200);
  auto const stacked = run_cpu(r, {.insn = svc, .unprivileged = true});
  REQUIRE(stacked.reason == stop_reason::fault);
  r.firewall->verdict = bus_verdict::allow;
  r.model->fail_reads = true;
  REQUIRE(r.f->target->mem().write(cfsr, 4, 0xffff'ffff));
  r.f->set("msp", 0x2008'4000);
  REQUIRE(r.f->target->mem().write(shcsr, 4, 0));
  auto const stopped = run_cpu(r, {.insn = load, .at = testdev + data});
  REQUIRE(stopped.reason == stop_reason::fault);
  REQUIRE(stopped.fault.has_value());
  CHECK(stopped.fault->status == cfsr_impreciserr);

  auto  read_leg             = booted(false, true);
  auto& services             = read_leg.model->context().services;
  read_leg.firewall->verdict = bus_verdict::bus_error;
  CHECK_FALSE(services.read(testdev + data, 4, initiator::cpu).has_value());
  read_leg.firewall->verdict = bus_verdict::allow;
  read_leg.model->fail_reads = true;
  auto const after_read      = run_cpu(read_leg, {.insn = load, .at = testdev + data});
  REQUIRE(after_read.reason == stop_reason::fault);
  REQUIRE(after_read.fault.has_value());
  CHECK(after_read.fault->status == cfsr_impreciserr);
}

TEST_CASE("a DMA access reaches the firewall and the model with the attributes it names")
{
  auto r = booted(true, true);
  CHECK(r.model->context().trustzone);
  auto& services = r.model->context().services;
  r.firewall->calls.clear();
  REQUIRE(services.write(testdev + data, 4, 7, bus_attrs{initiator::dma, false, true}));
  REQUIRE(r.firewall->calls.size() == 1);
  CHECK(r.firewall->calls[0].attrs == bus_attrs{initiator::dma, false, true});
  CHECK(last_ctx(r).by == initiator::dma);
  CHECK_FALSE(last_ctx(r).secure);
  CHECK(last_ctx(r).privileged);
  REQUIRE(services.read(sec_testdev + data, 4, bus_attrs{initiator::dma, true, false}));
  CHECK(r.firewall->calls[1].attrs == bus_attrs{initiator::dma, true, false});
  CHECK(last_ctx(r).secure);
  CHECK_FALSE(last_ctx(r).privileged);
  CHECK(last_ctx(r).via_secure_alias);
  r.firewall->verdict = bus_verdict::bus_error;
  CHECK(
      services.write(testdev + data, 4, 8, bus_attrs{initiator::dma, false, true}).error()
      == popcorn::bus_error::denied);
}

TEST_CASE(
    "exception stacking and unstacking carry the security and privilege of the stack, B3.19 Stack "
    "and PopStack")
{
  auto r              = booted(true, true);
  r.firewall->verdict = bus_verdict::read_zero_write_ignore;
  r.f->set("msp", testdev + 0x200);
  r.firewall->calls.clear();
  run_cpu(r, {.insn = svc, .unprivileged = true});
  auto const pushes =
      r.firewall->calls
      | std::views::filter([](auto const& call) { return call.op == memory_op::write; })
      | std::ranges::to<std::vector>();
  REQUIRE(pushes.size() == 8);
  for(auto const& push : pushes)
  {
    CHECK(push.unit == unit_of("TESTDEV"));
    CHECK(push.attrs == bus_attrs{.by = initiator::cpu, .secure = true, .privileged = false});
  }
  struct leg
  {
    std::uint32_t exc_return;
    bool          privileged;
  };
  for(auto const& one : {
          leg{exc_return_thread_msp,  false},
          leg{exc_return_handler_msp, true },
  })
  {
    CAPTURE(one.exc_return);
    REQUIRE(r.f->target->mem().write(code_at + 0x10, 2, bx_lr));
    r.f->set("control", 1);
    r.f->set("msp", testdev + 0x1e0);
    r.f->set("lr", one.exc_return);
    r.f->set("pc", (code_at + 0x10) | 1u);
    r.firewall->calls.clear();
    std::ignore = r.f->run({.max_instructions = icount{1}});
    auto const pops =
        r.firewall->calls
        | std::views::filter([](auto const& call) { return call.op == memory_op::read; })
        | std::ranges::to<std::vector>();
    REQUIRE(pops.size() == 8);
    for(auto const& pop : pops)
    {
      CHECK(pop.unit == unit_of("TESTDEV"));
      CHECK(
          pop.attrs
          == bus_attrs{.by = initiator::cpu, .secure = true, .privileged = one.privileged});
    }
    r                   = booted(true, true);
    r.firewall->verdict = bus_verdict::read_zero_write_ignore;
    r.f->set("msp", testdev + 0x200);
    run_cpu(r, {.insn = svc, .unprivileged = true});
  }
}

TEST_CASE(
    "Non-secure exception stacking and unstacking carry the Non-secure attribute and CONTROL_NS "
    "privilege, B3.19 Stack and PopStack")
{
  constexpr std::uint32_t exc_return_ns_thread  = 0xFFFF'FFB8;
  constexpr std::uint32_t exc_return_ns_handler = 0xFFFF'FFB0;
  struct leg
  {
    std::uint32_t exc_return;
    bool          privileged;
  };
  for(auto const& one : {
          leg{exc_return_ns_thread,  false},
          leg{exc_return_ns_handler, true },
  })
  {
    CAPTURE(one.exc_return);
    auto r              = booted(true, true);
    r.firewall->verdict = bus_verdict::read_zero_write_ignore;
    REQUIRE(r.f->target->mem().write(sau_ctrl, 4, sau_allns));
    engine_secure(r, false);
    r.f->set("msp", testdev + 0x200);
    r.f->set("control", 1);
    REQUIRE(r.f->target->mem().write(code_at, 2, svc));
    REQUIRE(r.f->target->mem().write(code_at + 2, 2, bkpt));
    r.f->set("pc", code_at | 1u);
    r.firewall->calls.clear();
    std::ignore = r.f->run({.max_instructions = icount{2}});
    auto const pushes =
        r.firewall->calls
        | std::views::filter([](auto const& call) { return call.op == memory_op::write; })
        | std::ranges::to<std::vector>();
    REQUIRE(pushes.size() == 8);
    for(auto const& push : pushes)
    {
      CHECK(push.unit == unit_of("TESTDEV"));
      CHECK(push.attrs == bus_attrs{.by = initiator::cpu, .secure = false, .privileged = false});
    }
    REQUIRE((r.f->reg("xpsr") & 0x1ffu) == 11);
    REQUIRE(r.f->target->mem().write(code_at + 0x10, 2, bx_lr));
    r.f->set("control", 1);
    r.f->set("msp", testdev + 0x1e0);
    r.f->set("lr", one.exc_return);
    r.f->set("pc", (code_at + 0x10) | 1u);
    r.firewall->calls.clear();
    std::ignore = r.f->run({.max_instructions = icount{1}});
    auto const pops =
        r.firewall->calls
        | std::views::filter([](auto const& call) { return call.op == memory_op::read; })
        | std::ranges::to<std::vector>();
    REQUIRE(pops.size() == 8);
    for(auto const& pop : pops)
    {
      CHECK(pop.unit == unit_of("TESTDEV"));
      CHECK(
          pop.attrs
          == bus_attrs{.by = initiator::cpu, .secure = false, .privileged = one.privileged});
    }
  }
}

TEST_CASE(
    "cross-state entry stacks with the background's attribute and reads the vector with the "
    "target's, B3.19 Stack and Vector[]")
{
  constexpr address vector_table = testdev + 0x100;
  struct leg
  {
    char const* name;
    bool        secure_background;
    std::size_t pushes;
  };
  for(auto const& one : {
          leg{"Non-secure thread, Secure IRQ", false, 8 },
          leg{"Secure thread, Non-secure IRQ", true,  18},
  })
  {
    CAPTURE(one.name);
    auto r              = booted(true, true);
    r.firewall->verdict = bus_verdict::read_zero_write_ignore;
    auto* const engine  = core::machine_internals::engine(*r.f->target);
    auto const  write   = [&](int id, std::uint32_t value) {
      REQUIRE(uc_reg_write(engine, id, &value) == UC_ERR_OK);
    };
    REQUIRE(r.f->target->mem().write(sau_ctrl, 4, sau_allns));
    REQUIRE(r.f->target->mem().write(0xE000'E100, 4, 1));
    if(one.secure_background)
    {
      REQUIRE(r.f->target->mem().write(0xE000'E380, 4, 1));
      REQUIRE(r.f->target->mem().write(0xE002'ED08, 4, vector_table));
      write(UC_ARM_REG_MSP_S, testdev + 0x200);
    }
    else
    {
      REQUIRE(r.f->target->mem().write(0xE000'ED08, 4, vector_table));
      write(UC_ARM_REG_MSP_NS, testdev + 0x200);
      write(UC_ARM_REG_CONTROL_NS, 1);
      engine_secure(r, false);
    }
    REQUIRE(r.f->target->mem().write(code_at, 2, bkpt));
    r.f->set("pc", code_at | 1u);
    REQUIRE(r.f->target->mem().write(0xE000'E200, 4, 1));
    r.firewall->calls.clear();
    std::ignore   = r.f->run({.max_instructions = icount{1}});
    auto const of = [&](memory_op op) {
      return r.firewall->calls
           | std::views::filter([op](auto const& call) { return call.op == op; })
           | std::ranges::to<std::vector>();
    };
    auto const pushes = of(memory_op::write);
    REQUIRE(pushes.size() == one.pushes);
    for(auto const& push : pushes)
    {
      CHECK(
          push.attrs
          == bus_attrs{
              .by         = initiator::cpu,
              .secure     = one.secure_background,
              .privileged = one.secure_background});
    }
    auto const reads = of(memory_op::read);
    REQUIRE(reads.size() == 1);
    CHECK(
        reads.front().attrs
        == bus_attrs{
            .by = initiator::cpu, .secure = not one.secure_background, .privileged = true});
  }
}

TEST_CASE("a second firewall is refused at machine build, and install_firewall after it")
{
  auto options = core::machine_options{};
  for(auto const name : {"UNMODELLED", "TESTDEV"})
  {
    options.models.emplace(
        name,
        [](periph::model_context& ctx, popcorn::peripheral_desc const&)
            -> std::shared_ptr<periph::model> { return std::make_shared<probe_firewall>(ctx); });
  }
  try
  {
    arch_fixture const built{m33_trustzone(), "scs", options};
    FAIL("the machine was built with two firewalls");
  }
  catch(popcorn::model_error const& error)
  {
    CHECK(error.peripheral() == "UNMODELLED");
  }
  auto r = booted(false, true);
  CHECK_THROWS_AS(r.model->context().install_firewall(*r.firewall), popcorn::model_error);
  REQUIRE(r.f->target->mem().write(testdev + data, 4, 9));
  CHECK(r.f->word(testdev + data) == 9);
  auto          plain = booted(false, false);
  idle_firewall late;
  CHECK_THROWS_AS(plain.model->context().install_firewall(late), popcorn::model_error);
  CHECK(plain.f->target->mem().write(testdev + data, 4, 10));
  CHECK(plain.f->word(testdev + data) == 10);
}

TEST_CASE("a factory that installs a firewall and returns no model is refused at machine build")
{
  static idle_firewall orphan;
  auto                 options = core::machine_options{};
  options.models.emplace(
      "UNMODELLED",
      [](periph::model_context& ctx,
         popcorn::peripheral_desc const&) -> std::shared_ptr<periph::model> {
        ctx.install_firewall(orphan);
        return nullptr;
      });
  try
  {
    arch_fixture const built{m33_trustzone(), "scs", options};
    FAIL("the machine was built with a firewall no model owns");
  }
  catch(popcorn::model_error const& error)
  {
    CHECK(error.peripheral() == "UNMODELLED");
  }
}

namespace
{
constexpr address       ccr           = 0xE000'ED14;
constexpr std::uint32_t ccr_bfhfnmign = 1u << 8;
constexpr address       unmapped      = 0x3000'0000;

auto run_code(rig const& r, std::initializer_list<std::uint16_t> code) -> popcorn::stop
{
  auto at = code_at;
  for(auto const insn : code)
  {
    REQUIRE(r.f->target->mem().write(at, 2, insn));
    at += 2;
  }
  r.f->set("pc", code_at | 1u);
  return r.f->run_to_stop({.max_instructions = icount{code.size() + 1}});
}

void set_bfhfnmign(rig const& r, bool value)
{
  auto const word = r.f->word(ccr);
  REQUIRE(r.f->target->mem().write(ccr, 4, value ? word | ccr_bfhfnmign : word & ~ccr_bfhfnmign));
}
}

TEST_CASE("a precise data BusFault under FAULTMASK with CCR.BFHFNMIGN set is ignored per E2.1.294")
{
  for(auto const policy :
      {popcorn::arch::fault_policy::stop, popcorn::arch::fault_policy::enter_handler})
  {
    for(auto const target : {testdev + data, unmapped})
    {
      for(auto const insn : {load, store})
      {
        CAPTURE(policy);
        CAPTURE(target);
        CAPTURE(insn);
        auto r = booted(false, true, policy);
        REQUIRE(r.f->target->mem().write(testdev + data, 4, 0x1234));
        r.firewall->verdict = bus_verdict::bus_error;
        REQUIRE(r.f->target->mem().write(shcsr, 4, 0x0002'0000));
        set_bfhfnmign(r, true);
        r.f->set("faultmask", 1);
        r.f->set("r0", target);
        r.f->set("r1", 0x77);
        auto const before  = r.model->accesses.size();
        auto const stopped = run_code(r, {insn, bkpt});
        REQUIRE(stopped.reason == stop_reason::user);
        CHECK(r.f->reg("pc") == code_at + 4);
        CHECK((r.f->reg("xpsr") & 0x1ff) == 0);
        CHECK(r.f->word(cfsr) == (cfsr_preciserr | cfsr_bfarvalid));
        CHECK(r.f->word(bfar) == target);
        CHECK(r.f->word(hfsr) == 0);
        CHECK_FALSE(r.f->module->pending());
        CHECK(r.f->reg("r1") == 0x77);
        CHECK(r.model->accesses.size() == before);
        r.firewall->verdict = bus_verdict::allow;
        CHECK(r.f->word(testdev + data) == 0x1234);
      }
    }
  }
}

TEST_CASE("a precise data BusFault is taken without FAULTMASK or without CCR.BFHFNMIGN")
{
  struct leg
  {
    bool                faultmask;
    bool                bfhfnmign;
    popcorn::fault_kind kind;
  };
  for(auto const one : {
          leg{true,  false, popcorn::fault_kind::hard},
          leg{false, true,  popcorn::fault_kind::bus }
  })
  {
    for(auto const target : {testdev + data, unmapped})
    {
      CAPTURE(one.faultmask);
      CAPTURE(target);
      auto r              = booted(false, true);
      r.firewall->verdict = bus_verdict::bus_error;
      REQUIRE(r.f->target->mem().write(shcsr, 4, 0x0002'0000));
      set_bfhfnmign(r, one.bfhfnmign);
      r.f->set("faultmask", one.faultmask ? 1 : 0);
      r.f->set("r0", target);
      auto const stopped = run_code(r, {load, bkpt});
      REQUIRE(stopped.reason == stop_reason::fault);
      REQUIRE(stopped.fault.has_value());
      CHECK(stopped.fault->kind == one.kind);
      CHECK(r.f->reg("pc") == code_at);
    }
  }
}

TEST_CASE("an ignored precise BusFault in an IT block advances ITSTATE past the skipped load")
{
  constexpr std::uint16_t cmp_r1_r1 = 0x4289;
  constexpr std::uint16_t ite_eq    = 0xbf0c;
  constexpr std::uint16_t mov_r2_5  = 0x2205;
  auto                    r         = booted(false, true);
  r.firewall->verdict               = bus_verdict::bus_error;
  set_bfhfnmign(r, true);
  r.f->set("faultmask", 1);
  r.f->set("r0", testdev + data);
  r.f->set("r1", 0x77);
  r.f->set("r2", 0);
  auto const stopped = run_code(r, {cmp_r1_r1, ite_eq, load, mov_r2_5, bkpt});
  REQUIRE(stopped.reason == stop_reason::user);
  CHECK(r.f->reg("pc") == code_at + 10);
  CHECK(r.f->reg("r1") == 0x77);
  CHECK(r.f->reg("r2") == 0);
  CHECK(r.f->word(cfsr) == (cfsr_preciserr | cfsr_bfarvalid));
}

TEST_CASE("an ignored precise BusFault of a 32-bit LDR.W resumes after its second halfword")
{
  constexpr std::uint16_t ldr_w_first  = 0xf8d0;
  constexpr std::uint16_t ldr_w_second = 0x1000;
  auto                    r            = booted(false, true);
  r.firewall->verdict                  = bus_verdict::bus_error;
  set_bfhfnmign(r, true);
  r.f->set("faultmask", 1);
  r.f->set("r0", testdev + data);
  r.f->set("r1", 0x77);
  r.f->set("r2", 0);
  auto const stopped = run_code(r, {ldr_w_first, ldr_w_second, bkpt});
  REQUIRE(stopped.reason == stop_reason::user);
  CHECK(r.f->reg("pc") == code_at + 6);
  CHECK(r.f->reg("r0") == testdev + data);
  CHECK(r.f->reg("r1") == 0x77);
  CHECK(r.f->word(bfar) == testdev + data);
}

TEST_CASE("an ignored precise BusFault keeps the base register write-back of its instruction")
{
  struct row
  {
    std::string_view           name;
    std::vector<std::uint16_t> code;
    std::string_view           base;
    address                    from;
    address                    to;
  };
  constexpr address      at = testdev + data;
  std::vector<row> const rows{
      {"STR.W post-indexed",    {0xf840, 0x1b04}, "r0",  at,     at + 4},
      {"LDR.W pre-indexed",     {0xf850, 0x1d08}, "r0",  at + 8, at    },
      {"STR.W offset",          {0xf8c0, 0x1000}, "r0",  at,     at    },
      {"STR.W negative offset", {0xf840, 0x1c04}, "r0",  at + 4, at + 4},
      {"STMIA T1",              {0xc006},         "r0",  at,     at + 8},
      {"LDMIA T1 base in list", {0xc803},         "r0",  at,     at    },
      {"LDMIA T1",              {0xc802},         "r0",  at,     at + 4},
      {"PUSH",                  {0xb402},         "msp", at + 4, at    },
      {"POP",                   {0xbc02},         "msp", at,     at + 4},
      {"LDMIA.W",               {0xe8b0, 0x0006}, "r0",  at,     at + 8},
      {"STMDB",                 {0xe920, 0x0006}, "r0",  at + 8, at    },
      {"LDRD post-indexed",     {0xe8f0, 0x1202}, "r0",  at,     at + 8},
      {"VSTMIA",                {0xeca0, 0x0a01}, "r0",  at,     at + 4},
  };
  for(auto const& one : rows)
  {
    CAPTURE(one.name);
    auto r              = booted(false, true);
    r.firewall->verdict = bus_verdict::bus_error;
    REQUIRE(r.f->target->mem().write(0xE000'ED88, 4, 0x00F0'0000));
    set_bfhfnmign(r, true);
    r.f->set("faultmask", 1);
    r.f->set("r0", 0);
    r.f->set(one.base, one.from);
    auto at_code = code_at;
    for(auto const insn : one.code)
    {
      REQUIRE(r.f->target->mem().write(at_code, 2, insn));
      at_code += 2;
    }
    REQUIRE(r.f->target->mem().write(at_code, 2, bkpt));
    r.f->set("pc", code_at | 1u);
    auto const msp     = r.f->reg("msp");
    auto const stopped = r.f->run_to_stop({.max_instructions = icount{3}});
    REQUIRE(stopped.reason == stop_reason::user);
    CHECK(r.f->reg("pc") == at_code + 2);
    CHECK(r.f->reg(one.base) == one.to);
    CHECK(
        (r.f->word(cfsr) & (cfsr_preciserr | cfsr_bfarvalid)) == (cfsr_preciserr | cfsr_bfarvalid));
    if(one.base != "msp")
    {
      CHECK(r.f->reg("msp") == msp);
    }
  }
}

TEST_CASE("a precise BusFault in the HardFault or NMI handler is ignored only with BFHFNMIGN set")
{
  constexpr address       handler      = code_at + 0x100;
  constexpr address       vector_table = code_at + 0x800;
  constexpr std::uint16_t udf          = 0xde00;
  struct leg
  {
    std::string_view            name;
    std::uint32_t               ipsr;
    bool                        bfhfnmign;
    popcorn::arch::fault_policy policy;
  };
  for(auto const& one : {
          leg{"HardFault ignores",  3, true,  popcorn::arch::fault_policy::enter_handler},
          leg{"HardFault locks up", 3, false, popcorn::arch::fault_policy::enter_handler},
          leg{"NMI ignores",        2, true,  popcorn::arch::fault_policy::enter_handler},
          leg{"NMI ignores, stop",  2, true,  popcorn::arch::fault_policy::stop         },
          leg{"NMI locks up",       2, false, popcorn::arch::fault_policy::enter_handler},
          leg{"NMI locks up, stop", 2, false, popcorn::arch::fault_policy::stop         },
  })
  {
    CAPTURE(one.name);
    auto r              = booted(false, true, one.policy);
    r.firewall->verdict = bus_verdict::bus_error;
    REQUIRE(r.f->target->mem().write(shcsr, 4, 0));
    REQUIRE(r.f->target->mem().write(vector_table + 4 * one.ipsr, 4, handler | 1u));
    REQUIRE(r.f->target->mem().write(0xE000'ED08, 4, vector_table));
    REQUIRE(r.f->target->mem().write(handler, 2, load));
    REQUIRE(r.f->target->mem().write(handler + 2, 2, bkpt));
    set_bfhfnmign(r, one.bfhfnmign);
    r.f->set("faultmask", 0);
    r.f->set("r0", testdev + data);
    r.f->set("r1", 0x77);
    if(one.ipsr == 3)
    {
      REQUIRE(r.f->target->mem().write(code_at, 2, udf));
    }
    else
    {
      REQUIRE(r.f->target->mem().write(code_at, 2, bkpt));
      REQUIRE(r.f->target->mem().write(0xE000'ED04, 4, 0x8000'0000));
    }
    r.f->set("pc", code_at | 1u);
    auto const stopped = r.f->run_to_stop({.max_instructions = icount{4}});
    CHECK((r.f->reg("xpsr") & 0x1ff) == one.ipsr);
    if(not one.bfhfnmign)
    {
      REQUIRE(stopped.reason == stop_reason::fault);
      CHECK(stopped.fault->kind == popcorn::fault_kind::hard);
      CHECK(r.f->reg("pc") == handler);
      continue;
    }
    REQUIRE(stopped.reason == stop_reason::user);
    CHECK(r.f->reg("pc") == handler + 4);
    CHECK(r.f->reg("r1") == 0x77);
    CHECK(
        (r.f->word(cfsr) & (cfsr_preciserr | cfsr_bfarvalid)) == (cfsr_preciserr | cfsr_bfarvalid));
    CHECK(r.f->word(bfar) == testdev + data);
  }
}

TEST_CASE("under TrustZone the requested priority of the access's own state decides BFHFNMIGN")
{
  namespace armv8m = popcorn::arch::armv8m;
  enum class active : std::uint8_t
  {
    none,
    hardfault_secure,
    hardfault_nonsecure,
    nmi,
  };
  struct leg
  {
    std::string_view name;
    bool             nonsecure;
    active           running;
    bool             faultmask_secure;
    bool             faultmask_nonsecure;
    bool             bfhfnmins;
    bool             ccr_nonsecure_alias;
    bool             ignored;
  };
  std::vector<leg> const legs{
      {"S FAULTMASK_S",                   false, active::none,                true,  false, false, false, true },
      {"S FAULTMASK_NS",                  false, active::none,                false, true,  false, false, false},
      {"S HardFault_S",                   false, active::hardfault_secure,    false, false, false, false, true },
      {"S HardFault_NS",                  false, active::hardfault_nonsecure, false, false, true,  false, false},
      {"S NMI to Secure",                 false, active::nmi,                 false, false, false, false, true },
      {"S NMI to Non-secure",             false, active::nmi,                 false, false, true,  false, false},
      {"NS FAULTMASK_NS",                 true,  active::none,                false, true,  false, false, true },
      {"NS FAULTMASK_S",                  true,  active::none,                true,  false, false, false, false},
      {"NS HardFault_NS",                 true,  active::hardfault_nonsecure, false, false, true,  false, true },
      {"NS HardFault_S",                  true,  active::hardfault_secure,    false, false, false, false, false},
      {"NS NMI to Non-secure",            true,  active::nmi,                 false, false, true,  false, true },
      {"NS NMI to Secure",                true,  active::nmi,                 false, false, false, false, false},
      {"NS alias CCR write, BFHFNMINS 0", true,  active::none,                false, true,  false, true,  false},
      {"NS alias CCR write, BFHFNMINS 1", true,  active::none,                false, true,  true,  true,  true },
  };
  for(auto const& one : legs)
  {
    CAPTURE(one.name);
    auto r              = booted(true, true);
    r.firewall->verdict = bus_verdict::bus_error;
    std::vector<std::byte> bytes;
    r.f->module->save(bytes);
    auto state                = reflex::serde::bson::deserializer{bytes}.load<armv8m::state>();
    state.scs.aircr_bfhfnmins = one.bfhfnmins;
    if(one.running == active::hardfault_secure)
    {
      armv8m::put_bit(state.exceptions.active, armv8m::exc_hardfault, true);
    }
    if(one.running == active::hardfault_nonsecure)
    {
      state.exceptions.nonsecure_active |= 1u << armv8m::exc_hardfault;
    }
    if(one.running == active::nmi)
    {
      armv8m::put_bit(state.exceptions.active, armv8m::exc_nmi, true);
    }
    std::vector<std::byte> edited;
    reflex::serde::bson::serializer{edited}.dump(state);
    r.f->module->load(edited);
    REQUIRE(r.f->target->mem().write(
        one.ccr_nonsecure_alias ? 0xE002'ED14 : ccr, 4, r.f->word(ccr) | ccr_bfhfnmign));
    auto* const engine = core::machine_internals::engine(*r.f->target);
    auto const  write  = [&](int id, std::uint32_t value) {
      REQUIRE(uc_reg_write(engine, id, &value) == UC_ERR_OK);
    };
    write(UC_ARM_REG_FAULTMASK_S, one.faultmask_secure ? 1 : 0);
    write(UC_ARM_REG_FAULTMASK_NS, one.faultmask_nonsecure ? 1 : 0);
    auto const stopped = run_cpu(
        r, {.insn      = load,
            .at        = testdev + data,
            .value     = 0x77,
            .nonsecure = one.nonsecure,
            .budget    = 3});
    if(one.ignored)
    {
      REQUIRE(stopped.reason == stop_reason::user);
      CHECK(r.f->reg("pc") == code_at + 4);
      CHECK(r.f->reg("r1") == 0x77);
      CHECK(
          (r.f->word(cfsr) & (cfsr_preciserr | cfsr_bfarvalid))
          == (cfsr_preciserr | cfsr_bfarvalid));
      CHECK(r.f->word(bfar) == testdev + data);
    }
    else
    {
      REQUIRE(stopped.reason == stop_reason::fault);
      CHECK(r.f->reg("pc") == code_at);
    }
  }
}
