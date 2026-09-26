#include "arch_fixture.hpp"

#include "cortex_m.hpp"
#include "state.hpp"

#include <popcorn/core/intercept.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/core/trace.hpp>
#include <popcorn/periph/model.hpp>
#include <popcorn/periph/register_file.hpp>

#include <doctest/doctest.h>

#include <reflex/serde/bson.hpp>

#include <cstdint>
#include <expected>
#include <memory>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

namespace
{
namespace core   = popcorn::core;
namespace armv8m = popcorn::arch::armv8m;
namespace periph = popcorn::periph;
using popcorn::address;
using popcorn::stop_reason;
using popcorn::testing::arch_fixture;
using popcorn::testing::m33_with;
using popcorn::testing::sym;
using popcorn::testing::words_at;

constexpr address ispr0 = 0xE000'E200;

auto fn(std::string_view name) -> address
{
  return sym("exceptions", name);
}

auto description() -> popcorn::mcu_description
{
  return m33_with([](popcorn::mcu_description& d) { d.core.num_irqs = 20; });
}

auto booted(core::machine_options options = {}) -> arch_fixture
{
  arch_fixture f{description(), "exceptions", std::move(options)};
  REQUIRE(f.run_to_stop().reason == stop_reason::user);
  return f;
}

auto log_of(arch_fixture const& f) -> std::vector<std::uint32_t>
{
  auto const count = f.word(fn("log_len"));
  return words_at(*f.target, fn("log_buf"), count);
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

auto run_to(arch_fixture const& f, std::string_view entry) -> popcorn::stop
{
  f.set("pc", fn(entry));
  return f.run_to_stop();
}

void pend(arch_fixture const& f, std::uint32_t bits)
{
  REQUIRE(f.target->mem().write(ispr0, 4, bits));
}

struct exceptions_result
{
  std::vector<std::uint32_t>      log;
  std::vector<std::uint64_t>      registers;
  std::vector<core::trace_record> irq_records;
  bool                            pending_left{};
};

auto run_exceptions() -> exceptions_result
{
  auto       f    = booted();
  auto const hook = f.target->add_intercept(fn("it_irq_label"), [](core::intercept_call const& c) {
    std::ignore = c.target().mem().write(ispr0, 4, 1u << 10);
    return core::intercept_result::proceed();
  });
  REQUIRE(run_to(f, "exceptions_run").reason == stop_reason::user);
  exceptions_result out{
      .log = log_of(f), .registers = register_values(f), .irq_records = {}, .pending_left = false};
  for(auto const& record : f.target->trace().records())
  {
    if(record.kind == core::trace_kind::irq)
    {
      out.irq_records.push_back(record);
    }
  }
  out.pending_left = f.module->pending();
  return out;
}

struct probe_case
{
  std::string_view entry;
  std::uint32_t    lr;
  std::uint32_t    frame;
  bool             process;
  std::uint32_t    control;
  bool             aligned_down;
  bool             fp;
};

class line_model final : public periph::model
{
public:
  static constexpr std::uint32_t data_offset = 0x58;
  static constexpr std::uint32_t ctrl_offset = 0x5c;

  explicit line_model(periph::model_context& ctx) noexcept : context_{ctx}
  {}

  [[nodiscard]] auto claims() const -> std::vector<std::string> override
  {
    return {"DATA", "CTRL"};
  }

  void reset(periph::register_file&, popcorn::reset_kind) override
  {}

  auto on_read(periph::register_file&, std::uint32_t offset, std::uint8_t, periph::access_ctx)
      -> std::expected<std::uint32_t, popcorn::bus_error> override
  {
    ++reads;
    if(offset == data_offset and repulse and reads == 1)
    {
      context_.irqs.front().raise();
      context_.irqs.front().lower();
    }
    if(offset == data_offset and not repulse and reads == 3)
    {
      context_.irqs.front().lower();
    }
    return 0u;
  }

  auto on_write(
      periph::register_file&,
      std::uint32_t offset,
      std::uint8_t,
      std::uint32_t,
      std::uint32_t value,
      periph::access_ctx) -> std::expected<void, popcorn::bus_error> override
  {
    if(offset == ctrl_offset)
    {
      context_.irqs.front().raise();
      if(value == 1)
      {
        context_.irqs.front().lower();
      }
    }
    return {};
  }

  void on_event(std::uint32_t, std::uint64_t) override
  {}

  void save(std::vector<std::byte>&) const override
  {}

  void load(std::span<std::byte const>) override
  {}

  int  reads   = 0;
  bool repulse = false;

private:
  periph::model_context& context_;
};

auto with_line_model() -> core::machine_options
{
  core::machine_options options;
  options.models.emplace(
      "TESTDEV",
      [](periph::model_context& ctx, popcorn::peripheral_desc const&)
          -> std::shared_ptr<periph::model> { return std::make_shared<line_model>(ctx); });
  return options;
}

auto saved_special(arch_fixture const& f) -> armv8m::special_registers
{
  std::vector<std::byte> bytes;
  f.module->save(bytes);
  return reflex::serde::bson::deserializer{bytes}.load<armv8m::state>().special;
}
}

TEST_CASE("the exception firmware logs nesting, tail-chains, masking, FP, PSP and grouping")
{
  auto const                       first = run_exceptions();
  auto const                       top   = fn("psp_stack_top");
  std::vector<std::uint32_t> const expected{
      0x100, 0x300, 0x310,      0x301,      0x3FF, 0x400, 0x401, 0x410, 0x4FF, 0x450,
      0x440, 0x4FE, 0x500,      0x501,      0x510, 0x502, 0x600, 0x610, 0x601, 0x710,
      0x701, 0x900, 0xFFFFFFFD, top - 0x20, 0x9FF, 0xC00, 0x300, 0x301, 0x310, 0xC01,
      0xD40, 0xD41, 0xD50,      0xD40,      0xD50, 0xD41, 0xA10, 0xA03};
  CHECK(first.log == expected);
  CHECK_FALSE(first.pending_left);
  REQUIRE(first.irq_records.size() >= 4);
  CHECK(first.irq_records[0].aux == 16);
  CHECK(first.irq_records[1].aux == 17);
  CHECK(first.irq_records[2].aux == 17);
  CHECK(first.irq_records[3].aux == 16);
  CHECK(first.irq_records[0].flags == 1);
  CHECK(first.irq_records[1].flags == 1);
  CHECK(first.irq_records[2].flags == 0);
  CHECK(first.irq_records[3].flags == 0);
  auto const second = run_exceptions();
  CHECK(second.log == first.log);
  CHECK(second.registers == first.registers);
}

TEST_CASE("entry stacks the frame and return restores every register")
{
  std::vector<probe_case> const cases{
      {"regs_thread_msp",        0xFFFFFFF9, 0x20, false, 0, false, false},
      {"regs_misaligned",        0xFFFFFFF9, 0x24, false, 0, true,  false},
      {"regs_thread_msp_fp",     0xFFFFFFE9, 0x68, false, 0, false, true },
      {"regs_thread_psp",        0xFFFFFFFD, 0x20, true,  0, false, false},
      {"regs_thread_psp_fp",     0xFFFFFFED, 0x68, true,  0, false, true },
      {"regs_nested",            0xFFFFFFF1, 0x20, false, 0, false, false},
      {"regs_nested_fp",         0xFFFFFFE1, 0x68, false, 0, false, true },
      {"regs_thread_psp_unpriv", 0xFFFFFFFD, 0x20, true,  1, false, false},
  };
  for(auto const& probe : cases)
  {
    CAPTURE(probe.entry);
    auto f = booted();
    REQUIRE(run_to(f, "probe_setup").reason == stop_reason::user);
    REQUIRE(run_to(f, probe.entry).reason == stop_reason::user);
    auto const before   = register_values(f);
    auto const special  = saved_special(f);
    auto const control2 = f.reg("control");
    auto const xpsr     = f.reg("xpsr");
    auto const used     = probe.process ? special.psp : special.msp;
    pend(f, 1u << 11);
    REQUIRE(f.run_to_stop().reason == stop_reason::user);
    CHECK(f.reg("lr") == probe.lr);
    CHECK((f.reg("xpsr") & 0x1ff) == 16 + 11);
    CHECK(f.reg("control") == probe.control);
    auto const inside = saved_special(f);
    auto const frame  = probe.process ? inside.psp : inside.msp;
    CHECK(frame == used - probe.frame);
    if(probe.process)
    {
      CHECK(inside.msp == special.msp);
    }
    auto const words = words_at(*f.target, frame, probe.frame / 4);
    CHECK(words[0] == before[0]);
    CHECK(words[1] == before[1]);
    CHECK(words[2] == before[2]);
    CHECK(words[3] == before[3]);
    CHECK(words[4] == before[12]);
    CHECK(words[5] == before[13]);
    CHECK(words[6] == before[14]);
    CHECK((words[7] & ~0x10'0200u) == xpsr);
    CHECK(((words[7] >> 20) & 1u) == ((control2 >> 3) & 1u));
    CHECK(((control2 >> 3) & 1u) == (probe.fp ? 1u : 0u));
    CHECK(((words[7] >> 9) & 1u) == (probe.aligned_down ? 1u : 0u));
    if(probe.fp)
    {
      for(std::size_t index = 0; index < 16; ++index)
      {
        CHECK(words[8 + index] == before[23 + index]);
      }
      CHECK(words[24] == before[55]);
    }
    REQUIRE(f.run_to_stop().reason == stop_reason::user);
    auto after = register_values(f);
    CHECK(after[14] == before[14] + 2);
    after[14] = before[14];
    if(probe.fp)
    {
      CHECK(after == before);
    }
    else
    {
      after.resize(23);
      CHECK(after == std::vector<std::uint64_t>(before.begin(), before.begin() + 23));
    }
    CHECK(f.reg("control") == control2);
    CHECK(saved_special(f) == special);
  }
}

TEST_CASE("a handler setting FAULTMASK returns to an unprivileged task with FAULTMASK clear")
{
  auto f = booted();
  REQUIRE(run_to(f, "probe_setup").reason == stop_reason::user);
  REQUIRE(run_to(f, "faultmask_to_unpriv").reason == stop_reason::user);
  CHECK(f.word(fn("faultmask_seen") + 4) == 3);
  CHECK(f.reg("faultmask") == 0);
  CHECK(f.reg("control") == 3);
  CHECK(saved_special(f).faultmask == 0);
}

TEST_CASE("a tail-chain runs no thread instruction between the two handlers")
{
  auto f = booted();
  REQUIRE(run_to(f, "probe_setup").reason == stop_reason::user);
  REQUIRE(run_to(f, "tail_chain").reason == stop_reason::user);
  pend(f, (1u << 14) | (1u << 15));
  REQUIRE(f.run_to_stop().reason == stop_reason::user);
  auto const lr_a  = f.reg("lr");
  auto const msp_a = f.reg("msp");
  CHECK(f.reg("r7") == 0);
  REQUIRE(f.run_to_stop().reason == stop_reason::user);
  CHECK((f.reg("xpsr") & 0x1ff) == 16 + 15);
  CHECK(f.reg("lr") == lr_a);
  CHECK(f.reg("msp") == msp_a);
  CHECK(f.reg("r7") == 0);
}

TEST_CASE("IRQ lines latch a pulse once and re-enter while held high")
{
  SUBCASE("pulse")
  {
    auto f = booted(with_line_model());
    REQUIRE(run_to(f, "probe_setup").reason == stop_reason::user);
    auto const before = log_of(f).size();
    REQUIRE(run_to(f, "latch_pulse").reason == stop_reason::user);
    auto const log = log_of(f);
    CHECK(std::ranges::count(log | std::views::drop(before), 0x330u) == 1);
  }
  SUBCASE("held")
  {
    auto f = booted(with_line_model());
    REQUIRE(run_to(f, "probe_setup").reason == stop_reason::user);
    auto const before = log_of(f).size();
    REQUIRE(run_to(f, "latch_held").reason == stop_reason::user);
    auto const log = log_of(f);
    CHECK(std::ranges::count(log | std::views::drop(before), 0x330u) == 3);
    CHECK_FALSE(f.module->pending());
  }
}

TEST_CASE("a pulse on a line whose IRQ is active pends it again")
{
  auto f = booted(with_line_model());
  REQUIRE(run_to(f, "probe_setup").reason == stop_reason::user);
  dynamic_cast<line_model&>(*f.target->model("TESTDEV")).repulse = true;
  auto const before                                              = log_of(f).size();
  REQUIRE(run_to(f, "latch_pulse").reason == stop_reason::user);
  auto const log = log_of(f);
  CHECK(std::ranges::count(log | std::views::drop(before), 0x330u) == 2);
}

TEST_CASE("ICPR leaves an IRQ pending while its line is high")
{
  auto f = booted(with_line_model());
  REQUIRE(f.target->mem().write(0xE000'E180, 4, 1u << 3));
  REQUIRE(f.target->mem().write(0x4001'005C, 4, 2));
  CHECK(f.target->mem().read(0xE000'E200, 4) == 1u << 3);
  REQUIRE(f.target->mem().write(0xE000'E280, 4, 1u << 3));
  CHECK(f.target->mem().read(0xE000'E200, 4) == 1u << 3);
}

TEST_CASE("ICSR reports the pending vector, pending IRQs and a return to base")
{
  auto f = booted();
  REQUIRE(run_to(f, "probe_setup").reason == stop_reason::user);
  REQUIRE(run_to(f, "icsr_probe").reason == stop_reason::user);
  auto const seen = words_at(*f.target, fn("icsr_seen"), 2);
  CHECK(seen[0] == (33u | 34u << 12 | 1u << 22 | 1u << 11));
  CHECK(seen[1] == (35u | 34u << 12 | 1u << 22));
}
