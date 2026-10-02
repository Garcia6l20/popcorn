#include <popcorn/arch/arch.hpp>
#include <popcorn/core/clock.hpp>
#include <popcorn/core/coverage.hpp>
#include <popcorn/core/machine.hpp>

#include "../support/machine_fixture.hpp"
#include "../support/test_arch.hpp"
#include "../support/test_model.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <ranges>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
namespace arch = popcorn::arch;
namespace core = popcorn::core;
using popcorn::address;
using popcorn::icount;
using popcorn::stop_reason;
using popcorn::testing::m33;
using popcorn::testing::sym;
using popcorn::testing::test_model;

constexpr address       testdev        = 0x4001'0000;
constexpr address       unmapped_page  = 0x3000'0000;
constexpr std::uint32_t handler_value  = 0xaa;
constexpr std::uint64_t handler_length = 4;

auto with_neighbour() -> popcorn::mcu_description
{
  auto desc = m33();
  auto own  = std::ranges::find(
      desc.peripherals, std::string_view{"TESTDEV"}, &popcorn::peripheral_desc::name);
  own->size = 0x100;
  auto next = *std::ranges::find(
      desc.peripherals, std::string_view{"UNMODELLED"}, &popcorn::peripheral_desc::name);
  next.name = "NEIGHBOUR";
  next.base = testdev + 0x100;
  next.size = 0x100;
  desc.peripherals.push_back(std::move(next));
  return desc;
}

auto testdev_at_page_end() -> popcorn::mcu_description
{
  auto desc = m33();
  auto own  = std::ranges::find(
      desc.peripherals, std::string_view{"TESTDEV"}, &popcorn::peripheral_desc::name);
  own->base = testdev + 0x3a0;
  own->size = 0x60;
  auto next = *std::ranges::find(
      desc.peripherals, std::string_view{"UNMODELLED"}, &popcorn::peripheral_desc::name);
  next.name = "NEIGHBOUR";
  next.base = testdev;
  next.size = 0x100;
  desc.peripherals.push_back(std::move(next));
  return desc;
}

struct fixture : popcorn::testing::machine_fixture<>
{
  explicit fixture(popcorn::mcu_description desc = m33())
      : popcorn::testing::machine_fixture<>{std::move(desc), "irq", options()}
  {
    REQUIRE(run_to_stop().reason == stop_reason::user);
  }

  static auto options() -> core::machine_options
  {
    core::machine_options made;
    made.models.emplace("TESTDEV", popcorn::testing::test_model_factory());
    return made;
  }

  [[nodiscard]] auto model() const -> test_model&
  {
    return dynamic_cast<test_model&>(*target->model("TESTDEV"));
  }

  void enter(std::string_view entry) const
  {
    set("pc", sym("irq", entry));
    set("r1", 1);
    set("r2", testdev);
    set("r3", handler_value);
    set("r8", unmapped_page);
    auto const target = entry == "jump_ahead" ? "jump_ahead_target"
                      : entry == "jump_mid"   ? "jump_mid_target"
                                              : "jump_back";
    model().context().regs.poke(test_model::ctrl_offset, sym("irq", target) | 1u);
    for(auto const name : {"r4", "r5", "r6", "r7"})
    {
      set(name, 0);
    }
  }

  [[nodiscard]] auto now() const -> std::uint64_t
  {
    return target->virtual_clock().now().value;
  }

  [[nodiscard]] auto data_writes() const -> std::vector<std::uint32_t>
  {
    std::vector<std::uint32_t> values;
    for(auto const& access : model().accesses)
    {
      if(access.write and access.offset == test_model::data_offset)
      {
        values.push_back(access.value);
      }
    }
    return values;
  }
};

auto raising(popcorn::mcu_description desc = m33()) -> fixture
{
  fixture f{std::move(desc)};
  f.model().raise_on_go        = true;
  f.model().raise_on_ctrl_read = true;
  f.model().go_delay           = 1'000'000;
  return f;
}

auto quiet() -> fixture
{
  fixture f;
  f.model().go_delay = 1'000'000;
  return f;
}

auto executed_from(fixture const& f, std::string_view entry) -> std::uint64_t
{
  f.enter(entry);
  auto const result = f.run({.wall_budget = std::chrono::seconds{60}});
  REQUIRE(result.stopped.has_value());
  REQUIRE(result.stopped->reason == stop_reason::user);
  return result.executed.value;
}
}

TEST_CASE("a store that raises an unmasked IRQ takes it before the next instruction")
{
  auto f = raising();
  f.model().accesses.clear();
  f.enter("store_raise");
  CHECK(f.run_to_stop().reason == stop_reason::user);
  CHECK(f.reg("r5") == 0);
  CHECK(f.data_writes() == std::vector<std::uint32_t>{handler_value, 1});
}

TEST_CASE("a load that raises an unmasked IRQ takes it before the next instruction")
{
  auto f = raising();
  f.enter("read_raise");
  CHECK(f.run_to_stop().reason == stop_reason::user);
  CHECK(f.reg("r5") == 0);
  CHECK(f.data_writes() == std::vector<std::uint32_t>{handler_value, 1});
}

TEST_CASE("a store or load to a page shared with another peripheral takes the IRQ at once")
{
  auto f = raising(with_neighbour());
  SUBCASE("store")
  {
    f.enter("store_raise");
  }
  SUBCASE("load")
  {
    f.enter("read_raise");
  }
  CHECK(f.run_to_stop().reason == stop_reason::user);
  CHECK(f.reg("r5") == 0);
  CHECK(f.data_writes() == std::vector<std::uint32_t>{handler_value, 1});
}

TEST_CASE("a line raised and lowered within one access still pends the IRQ and exits")
{
  auto f          = raising();
  f.model().pulse = true;
  f.enter("store_raise");
  CHECK(f.run_to_stop().reason == stop_reason::user);
  CHECK(f.reg("r5") == 0);
  CHECK(f.data_writes() == std::vector<std::uint32_t>{handler_value, 1});
}

TEST_CASE("with PRIMASK set the IRQ waits for the CPSIE, as without a precise exit")
{
  auto f = raising();
  f.enter("masked_raise");
  CHECK(f.run_to_stop().reason == stop_reason::user);
  CHECK(f.reg("r5") == 1);
  CHECK(f.reg("r7") == 0);
  CHECK(f.data_writes() == std::vector<std::uint32_t>{1, handler_value});
}

TEST_CASE("a store inside an IT block takes the IRQ right after the block")
{
  auto f = raising();
  f.enter("it_raise");
  CHECK(f.run_to_stop().reason == stop_reason::user);
  CHECK(f.reg("r5") == 1);
  CHECK(f.reg("r7") == 0);
  CHECK(f.data_writes() == std::vector<std::uint32_t>{handler_value, 1});
}

TEST_CASE("instructions counted across a precise exit are the instructions run")
{
  for(auto const entry :
      {"store_raise", "read_raise", "it_raise", "jump_back", "jump_mid", "jump_ahead"})
  {
    CAPTURE(entry);
    auto const plain = executed_from(quiet(), entry);
    auto       f     = raising();
    auto const start = f.now();
    auto const ran   = executed_from(f, entry);
    CHECK(ran == plain + handler_length);
    CHECK(f.now() - start == ran);
  }
}

TEST_CASE("a model access right after a precise exit never reads an earlier count")
{
  auto f = raising();
  f.model().accesses.clear();
  f.enter("store_raise");
  CHECK(f.run_to_stop().reason == stop_reason::user);
  auto const& accesses = f.model().accesses;
  REQUIRE(accesses.size() == 3);
  CHECK(accesses[1].ctx.now >= accesses[0].ctx.now);
  CHECK(accesses[2].ctx.now >= accesses[1].ctx.now);
}

TEST_CASE("a budget counts from the exact count after a precise exit, not the held one")
{
  auto f = raising();
  f.enter("store_raise");
  auto const result = f.run({.wall_budget = std::chrono::seconds{60}, .budget_left = icount{10}});
  REQUIRE(result.stopped.has_value());
  CHECK(result.stopped->reason == stop_reason::user);
  CHECK(f.reg("r5") == 0);
}

TEST_CASE("an event due inside the held count fires once execution reaches it")
{
  auto f                = raising();
  f.model().raise_on_go = false;
  f.model().go_delay    = 5;
  REQUIRE(f.target->mem().write(testdev + test_model::ctrl_offset, 4, 1));
  f.model().raise_on_go = true;
  f.model().accesses.clear();
  f.enter("store_raise");
  auto const& model  = f.model();
  auto const  result = f.run({.wall_budget = std::chrono::seconds{60}, .stop_when = [&model] {
                               return model.fired_at.has_value();
                              }});
  REQUIRE(result.stopped.has_value());
  CHECK(model.fired_at.has_value());
  CHECK(model.accesses.size() == 2);
  CHECK(f.reg("r5") == 0);
}

TEST_CASE("a precise exit taken after a branch at the end of its block records no coverage cut")
{
  for(auto const entry : {"jump_back", "jump_mid", "jump_ahead"})
  {
    CAPTURE(entry);
    auto f = raising();
    f.target->enable_coverage({});
    f.enter(entry);
    CHECK(f.run_to_stop().reason == stop_reason::user);
    CHECK(f.reg("r5") == 1);
    CHECK(f.target->coverage().cuts.empty());
  }
}

TEST_CASE("a precise exit taken while stepping records its coverage cut")
{
  auto f = raising();
  f.target->enable_coverage({});
  f.enter("store_raise");
  auto const result = f.run(
      {.wall_budget = std::chrono::seconds{60}, .exact_instructions = icount{1 + handler_length}});
  CHECK(result.executed == icount{1 + handler_length});
  CHECK(
      f.target->coverage().cuts
      == std::vector<core::cut_counter>{
          {.start = sym("irq", "store_raise"), .at = sym("irq", "store_raise_next"), .hits = 1}
  });
}

TEST_CASE("a run after a precise exit records its own coverage cuts")
{
  auto f = raising();
  f.target->enable_coverage({});
  f.enter("store_raise");
  CHECK(f.run_to_stop().reason == stop_reason::user);
  f.target->clear_coverage();
  f.enter("store_raise_next");
  auto const until  = sym("irq", "store_raise_done") - 4;
  auto const result = f.run({.wall_budget = std::chrono::seconds{60}, .until = until});
  REQUIRE(result.stopped.has_value());
  CHECK(result.stopped->where.pc == until);
  CHECK(
      f.target->coverage().cuts
      == std::vector<core::cut_counter>{
          {.start = sym("irq", "store_raise_next"), .at = until, .hits = 1}
  });
}

TEST_CASE("a rate change in the raising store's block keeps model time monotonic")
{
  auto plain                     = quiet();
  plain.model().halve_rate_on_go = true;
  auto const without             = executed_from(plain, "store_raise");

  auto f                     = raising();
  f.model().halve_rate_on_go = true;
  f.model().accesses.clear();
  auto const ran = executed_from(f, "store_raise");
  CHECK(ran == without + handler_length + 13);
  auto const& accesses = f.model().accesses;
  REQUIRE(accesses.size() == 3);
  CHECK(accesses[1].ctx.now >= accesses[0].ctx.now);
  CHECK(accesses[2].ctx.now >= accesses[1].ctx.now);
  REQUIRE(f.model().go_time.has_value());
  CHECK(f.target->virtual_clock().time() >= *f.model().go_time);
  CHECK(f.target->virtual_clock().time().ps < 1'000'000'000'000ull);
}

TEST_CASE("a fault in the raising instruction keeps the coverage cut of the fault")
{
  auto       f    = raising(testdev_at_page_end());
  auto const base = testdev + 0x3a0;
  f.target->enable_coverage({});
  f.enter("stm_fault");
  f.set("r2", base);
  f.set("r6", base + test_model::ctrl_offset);
  auto const result = f.run({.wall_budget = std::chrono::seconds{60}});
  REQUIRE(result.stopped.has_value());
  CHECK(result.stopped->reason == stop_reason::fault);
  REQUIRE(result.stopped->fault.has_value());
  CHECK(result.stopped->fault->fault_address == testdev + 0x400);
  CHECK(
      f.target->coverage().cuts
      == std::vector<core::cut_counter>{
          {.start = sym("irq", "stm_fault"), .at = sym("irq", "stm_fault_at"), .hits = 1}
  });
}

TEST_CASE("coverage records a cut where a precise exit leaves a block")
{
  auto f = raising();
  f.target->enable_coverage({});
  f.enter("store_raise");
  CHECK(f.run_to_stop().reason == stop_reason::user);
  CHECK(
      f.target->coverage().cuts
      == std::vector<core::cut_counter>{
          {.start = sym("irq", "store_raise"), .at = sym("irq", "store_raise_next"), .hits = 1}
  });
}

TEST_CASE("an exact instruction stop single-steps through a precise exit without a refund")
{
  auto f                = raising();
  f.model().raise_on_go = false;
  CHECK(executed_from(f, "store_raise") > 1);
  f.model().raise_on_go = true;
  f.enter("store_raise");
  auto const exact  = icount{1 + handler_length + 1};
  auto const result = f.run({.wall_budget = std::chrono::seconds{60}, .exact_instructions = exact});
  CHECK(result.executed == exact);
  CHECK(f.reg("r5") == 0);
  CHECK(f.reg("r4") == 1);
  CHECK(f.reg("pc") == sym("irq", "store_raise_next") + 2);
}

TEST_CASE("a model stop requested by the raising store ends the slice right after it")
{
  auto f                  = raising();
  f.model().stop_on_write = true;
  f.model().accesses.clear();
  f.enter("store_raise");
  auto const stopped = f.run_to_stop();
  CHECK(stopped.reason == stop_reason::user);
  CHECK(stopped.detail == "model stop");
  CHECK(stopped.where.pc == sym("irq", "store_raise_next"));
  CHECK(f.reg("r4") == 0);
  CHECK(f.model().accesses.size() == 1);

  f.model().stop_on_write = false;
  CHECK(f.run_to_stop().reason == stop_reason::user);
  CHECK(f.reg("r5") == 0);
  CHECK(f.data_writes() == std::vector<std::uint32_t>{handler_value, 1});
}

TEST_CASE("a store fault after the raising access in its IT block counts the block up to its end")
{
  auto f = raising();
  f.enter("it_fault");
  auto const result = f.run({.wall_budget = std::chrono::seconds{60}});
  REQUIRE(result.stopped.has_value());
  CHECK(result.stopped->reason == stop_reason::fault);
  REQUIRE(result.stopped->fault.has_value());
  CHECK(result.stopped->fault->fault_address == unmapped_page);
  CHECK(result.executed.value == 4);
}

TEST_CASE("a snapshot taken at a precise exit stop replays the same run after a restore")
{
  auto f                  = raising();
  f.model().stop_on_write = true;
  f.enter("store_raise");
  REQUIRE(f.run_to_stop().where.pc == sym("irq", "store_raise_next"));
  f.model().stop_on_write = false;
  auto const saved        = f.target->snapshot();
  auto const held         = f.now();

  f.model().accesses.clear();
  CHECK(f.run_to_stop().reason == stop_reason::user);
  auto const continuous = f.model().accesses;
  auto const end        = f.now();

  f.target->restore(saved);
  CHECK(f.now() == held);
  f.model().accesses.clear();
  CHECK(f.run_to_stop().reason == stop_reason::user);
  auto const& replayed = f.model().accesses;
  CHECK(f.now() == end);
  REQUIRE(replayed.size() == continuous.size());
  REQUIRE(continuous.size() == 2);
  for(auto const [left, right] : std::views::zip(continuous, replayed))
  {
    CHECK(left.ctx.now == right.ctx.now);
    CHECK(left.value == right.value);
  }
}

TEST_CASE("a reset at a precise exit stop restarts the clock at 0, nothing held")
{
  auto f                  = raising();
  f.model().stop_on_write = true;
  f.enter("store_raise");
  REQUIRE(f.run_to_stop().where.pc == sym("irq", "store_raise_next"));
  REQUIRE(f.now() != 0);
  f.target->reset();
  CHECK(f.now() == 0);
  CHECK(f.target->virtual_clock().time() == popcorn::vtime{0});
}

TEST_CASE("a call made at a precise exit stop returns, and the run then takes the IRQ first")
{
  auto f                  = raising();
  f.model().stop_on_write = true;
  f.enter("store_raise");
  REQUIRE(f.run_to_stop().where.pc == sym("irq", "store_raise_next"));
  f.model().stop_on_write = false;

  auto const result = f.target->call(
      {
          .entry           = sym("irq", "twice"),
          .args            = {{.kind = arch::value_kind::i32, .bits = 21}},
          .returns         = arch::value_kind::i32,
          .abi             = arch::float_abi::soft,
          .return_sentinel = unmapped_page,
      },
      icount{1000});
  CHECK(result.value == 42);
  CHECK(f.reg("pc") == sym("irq", "store_raise_next"));

  f.model().accesses.clear();
  CHECK(f.run_to_stop().reason == stop_reason::user);
  CHECK(f.reg("r5") == 0);
  CHECK(f.data_writes() == std::vector<std::uint32_t>{handler_value, 1});
}
