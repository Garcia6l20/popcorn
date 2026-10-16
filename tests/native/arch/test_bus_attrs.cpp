#include "arch_fixture.hpp"

#include "../../../native/src/core/machine_impl.hpp"
#include "../support/test_model.hpp"

#include <popcorn/core/errors.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/periph/firewall.hpp>

#include <doctest/doctest.h>

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

class probe_firewall final : public periph::model, public periph::bus_firewall
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

  [[nodiscard]] auto claims() const -> std::vector<std::string> override
  {
    return {};
  }
  void reset(periph::register_file&, popcorn::reset_kind) override
  {}
  auto on_read(periph::register_file&, std::uint32_t, std::uint8_t, periph::access_ctx)
      -> std::expected<std::uint32_t, popcorn::bus_error> override
  {
    return 0u;
  }
  auto on_write(
      periph::register_file&,
      std::uint32_t,
      std::uint8_t,
      std::uint32_t,
      std::uint32_t,
      periph::access_ctx) -> std::expected<void, popcorn::bus_error> override
  {
    return {};
  }
  void on_event(std::uint32_t, std::uint64_t) override
  {}
  void save(std::vector<std::byte>&) const override
  {}
  void load(std::span<std::byte const>) override
  {}

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

auto booted(bool trustzone, bool firewall) -> rig
{
  rig  out;
  auto options = trustzone ? trustzone_options() : core::machine_options{};
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
  auto const stopped = r.f->run_to_stop({.max_instructions = icount{2}});
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
  CHECK(stopped.reason == stop_reason::fault);
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

TEST_CASE("a firewall bus_error is the BusFault of a model bus error, imprecise as today, B3.27")
{
  auto r              = booted(false, true);
  r.firewall->verdict = bus_verdict::bus_error;
  auto const before   = r.model->accesses.size();
  auto const stopped  = run_cpu(r, {.insn = load, .at = testdev + data});
  REQUIRE(stopped.reason == stop_reason::fault);
  REQUIRE(stopped.fault.has_value());
  CHECK(stopped.fault->kind == popcorn::fault_kind::bus);
  CHECK(stopped.fault->fault_address == testdev + data);
  CHECK(stopped.fault->status == cfsr_impreciserr);
  CHECK(r.model->accesses.size() == before);
  CHECK_FALSE(r.f->target->mem().read(testdev + data, 4).has_value());
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
