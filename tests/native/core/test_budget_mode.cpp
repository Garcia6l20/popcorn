#include "../../../native/src/core/machine_impl.hpp"

#include <popcorn/core/clock.hpp>
#include <popcorn/core/machine.hpp>

#include "../support/machine_fixture.hpp"
#include "../support/test_arch.hpp"
#include "../support/test_model.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <random>
#include <span>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

namespace
{
namespace core = popcorn::core;
using popcorn::address;
using popcorn::icount;
using popcorn::stop_reason;
using popcorn::testing::m33;
using popcorn::testing::sym;
using popcorn::testing::test_model;

constexpr address       testdev       = 0x4001'0000;
constexpr address       unmapped_page = 0x3000'0000;
constexpr std::uint32_t handler_value = 0xaa;
constexpr std::int64_t  caps[]        = {1, 2, 7, 65536};

struct fixture : popcorn::testing::machine_fixture<>
{
  explicit fixture(std::string_view firmware = "irq", bool drives_clock = false)
      : popcorn::testing::machine_fixture<>{m33(), firmware, options(drives_clock)}
  {
    if(firmware == "irq")
    {
      REQUIRE(run_to_stop().reason == stop_reason::user);
    }
  }

  static auto options(bool drives_clock) -> core::machine_options
  {
    core::machine_options made;
    made.models.emplace(
        "TESTDEV",
        [drives_clock](popcorn::periph::model_context& ctx, popcorn::peripheral_desc const&)
            -> std::shared_ptr<popcorn::periph::model> {
          auto built          = std::make_shared<test_model>(ctx);
          built->drives_clock = drives_clock;
          return built;
        });
    return made;
  }

  [[nodiscard]] auto model() const -> test_model&
  {
    return dynamic_cast<test_model&>(*target->model("TESTDEV"));
  }

  [[nodiscard]] auto counters() const -> core::run_counters const&
  {
    return core::machine_internals::counters(*target);
  }

  void enter(std::string_view entry) const
  {
    set("pc", sym("irq", entry));
    set("r1", 1);
    set("r2", testdev);
    set("r3", handler_value);
    set("r8", unmapped_page);
    auto const jump = entry == "jump_ahead" ? "jump_ahead_target"
                    : entry == "jump_mid"   ? "jump_mid_target"
                                            : "jump_back";
    model().context().regs.poke(test_model::ctrl_offset, sym("irq", jump) | 1u);
    for(auto const name : {"r4", "r5", "r6", "r7"})
    {
      set(name, 0);
    }
  }
};

struct access_seen
{
  std::uint32_t offset{};
  bool          write{};
  std::uint32_t value{};
  address       pc{};
  icount        now{};

  friend auto operator==(access_seen const&, access_seen const&) -> bool = default;
};

struct outcome
{
  stop_reason                reason{};
  address                    pc{};
  std::uint64_t              executed{};
  std::uint64_t              now{};
  std::uint64_t              ps{};
  std::vector<std::uint32_t> registers;
  std::vector<access_seen>   accesses;
  std::optional<icount>      fired_at;

  friend auto operator==(outcome const&, outcome const&) -> bool = default;
};

enum class setup : std::uint8_t
{
  quiet,
  raising,
  rate,
};

auto run_entry(std::string_view entry, setup how, std::optional<std::int64_t> cap) -> outcome
{
  fixture f{"irq", how == setup::rate};
  auto&   model = f.model();
  if(cap)
  {
    core::machine_internals::set_budget_cap(*f.target, *cap);
  }
  else
  {
    core::machine_internals::set_budget_off(*f.target, true);
  }
  model.raise_on_go        = how == setup::raising;
  model.raise_on_ctrl_read = how == setup::raising;
  model.halve_rate_on_go   = how == setup::rate;
  model.go_delay           = 1'000'000;
  f.enter(entry);
  model.record = true;
  model.accesses.clear();
  auto const budget_runs = f.counters().budget_runs;
  auto const start       = f.target->virtual_clock().now().value;
  auto const result      = f.run({.wall_budget = std::chrono::seconds{2}});
  REQUIRE(result.stopped.has_value());
  CHECK((f.counters().budget_runs > budget_runs) == cap.has_value());
  outcome made{
      .reason    = result.stopped->reason,
      .pc        = result.stopped->where.pc,
      .executed  = result.executed.value,
      .now       = f.target->virtual_clock().now().value - start,
      .ps        = f.target->virtual_clock().time().ps,
      .registers = {},
      .accesses  = {},
      .fired_at  = model.fired_at,
  };
  for(auto const name : {"r4", "r5", "r6", "r7"})
  {
    made.registers.push_back(static_cast<std::uint32_t>(f.reg(name)));
  }
  for(auto const& access : model.accesses)
  {
    made.accesses.push_back(
        {.offset = access.offset,
         .write  = access.write,
         .value  = access.value,
         .pc     = access.ctx.pc,
         .now    = access.ctx.now});
  }
  return made;
}

auto copy_entry() -> address
{
  return sym("fnv", "copy_words") + 4;
}
}

TEST_CASE("model accesses, precise exits and rate changes match the live hook at any budget cap")
{
  for(auto const entry :
      {"store_raise", "read_raise", "masked_raise", "it_raise", "jump_back", "jump_mid",
       "jump_ahead"})
  {
    for(auto const how : {setup::quiet, setup::raising, setup::rate})
    {
      CAPTURE(entry);
      CAPTURE(std::to_underlying(how));
      auto const live = run_entry(entry, how, std::nullopt);
      for(auto const cap : caps)
      {
        CAPTURE(cap);
        CHECK(run_entry(entry, how, cap) == live);
      }
    }
  }
}

TEST_CASE("an interrupt from another thread ends a budget run at a block start, 1000 times")
{
  fixture      f{"fnv"};
  auto const   head = copy_entry();
  auto const   body = head + 6;
  auto const   done = head + 4;
  std::mt19937 random{20261115};
  std::size_t  interrupted = 0;
  for(int round = 0; round < 1000; ++round)
  {
    CAPTURE(round);
    f.set("pc", head);
    f.set("r0", sym("fnv", "fnv_copy"));
    f.set("r1", 0x0800'0000);
    f.set("r2", 16384);
    f.set("r3", 0);
    auto const   delay = std::chrono::microseconds{random() % 100};
    std::jthread other{[&f, delay] {
      std::this_thread::sleep_for(delay);
      f.target->interrupt();
    }};
    auto const   runs  = f.counters().engine_runs;
    auto const   slice = f.run({.wall_budget = std::chrono::seconds{2}, .until = done});
    other.join();
    CHECK(f.counters().engine_runs - runs < 16);
    auto const pc = static_cast<address>(f.reg("pc"));
    if(slice.stopped)
    {
      CHECK(slice.stopped->reason == stop_reason::address);
      CHECK(pc == done);
      continue;
    }
    auto const executed = slice.executed.value;
    interrupted += executed != 0 ? 1 : 0;
    if(pc == head)
    {
      CHECK(executed % 6 == 0);
    }
    else
    {
      CHECK((pc == body or pc == done));
      CHECK(executed % 6 == 2);
    }
  }
  CHECK(interrupted > 0);
  CHECK(f.counters().budget_runs > 0);
}

TEST_CASE(
    "a model access and its log in a later block of a budget run carry the start of its block")
{
  for(auto const live : {false, true})
  {
    CAPTURE(live);
    fixture f{"fnv"};
    core::machine_internals::set_budget_off(*f.target, live);
    auto const head = copy_entry();
    f.set("pc", head);
    f.set("r0", testdev + test_model::data_offset);
    f.set("r1", 0x0800'0000);
    f.set("r2", 2);
    f.set("r3", 0);
    f.model().record       = true;
    f.model().log_messages = true;
    f.model().accesses.clear();
    std::vector<core::log_record> records;
    auto const                    sink =
        f.target->add_log_sink([&](core::log_record const& r) { records.push_back(r); });
    auto const stopped = f.run_to_stop({.until = head + 4});
    REQUIRE(stopped.reason == stop_reason::address);
    REQUIRE(f.model().accesses.size() == 2);
    CHECK(f.model().accesses[0].ctx.pc == head + 6);
    CHECK(f.model().accesses[1].ctx.pc == head + 6);
    REQUIRE(records.size() >= 2);
    CHECK(records[0].message == "write 0x58");
    CHECK(records[0].pc == head + 6);
    CHECK(records[1].message == "write 0x5c");
    CHECK(records[1].pc == head + 6);
  }
}

TEST_CASE("an interrupt from another thread ends a live coverage run at a block start, edges kept")
{
  fixture    f{"fnv"};
  auto const head = copy_entry();
  auto const body = head + 6;
  auto const done = head + 4;
  f.target->enable_coverage({
      {.name = "copy", .start = head, .size = 0x10}
  });
  std::mt19937  random{20261116};
  std::uint64_t starts      = 0;
  std::size_t   interrupted = 0;
  bool          fresh       = false;
  for(int round = 0; round < 100; ++round)
  {
    CAPTURE(round);
    if(round == 0 or static_cast<address>(f.reg("pc")) == done)
    {
      f.set("pc", head);
      f.set("r0", sym("fnv", "fnv_copy"));
      f.set("r1", 0x0800'0000);
      f.set("r2", 16384);
      f.set("r3", 0);
      fresh = true;
    }
    auto const   delay = std::chrono::microseconds{random() % 200};
    std::jthread other{[&f, delay] {
      std::this_thread::sleep_for(delay);
      f.target->interrupt();
    }};
    auto const   slice = f.run({.wall_budget = std::chrono::seconds{60}, .until = done});
    other.join();
    if(fresh and slice.executed.value != 0)
    {
      ++starts;
      fresh = false;
    }
    auto const pc = static_cast<address>(f.reg("pc"));
    if(slice.stopped)
    {
      CHECK(pc == done);
      std::ignore = f.run({.max_instructions = icount{0}, .wall_budget = std::chrono::seconds{60}});
      continue;
    }
    interrupted += slice.executed.value != 0 ? 1 : 0;
    CAPTURE(pc);
    CHECK((pc == head or pc == body or pc == done));
  }
  CHECK(interrupted > 0);
  auto const data = f.target->coverage();
  REQUIRE(data.blocks.size() == 1);
  auto const& hits      = data.blocks.front().hits;
  auto const  head_hits = std::uint64_t{hits[0]};
  auto const  body_hits = std::uint64_t{hits[3]};
  auto const  edge      = [&data](address from, address to) {
    auto const found = std::ranges::find_if(
        data.edges, [&](core::edge_counter const& e) { return e.from == from and e.to == to; });
    return found == data.edges.end() ? std::uint64_t{} : found->hits;
  };
  CHECK(body_hits > 0);
  CHECK(edge(head, body) == body_hits);
  CHECK(edge(body, head) + starts == head_hits);
  CHECK(data.cuts.empty());
}

TEST_CASE("a host read of an engine window between slices moves no clock and arms nothing")
{
  fixture f;
  f.enter("store_raise");
  std::ignore         = f.run_to_stop();
  auto const before   = f.target->virtual_clock().now();
  auto const counters = f.counters();
  for(int i = 0; i < 3; ++i)
  {
    std::ignore = f.target->mem().read(testdev + test_model::ctrl_offset, 4);
  }
  CHECK(f.target->virtual_clock().now() == before);
  CHECK(f.counters().engine_runs == counters.engine_runs);
  CHECK(f.counters().budget_runs == counters.budget_runs);
}

TEST_CASE("an IRQ pended while masked is taken at the cpsie two blocks later, budget or live")
{
  constexpr address                       code = 0x2004'0000;
  constexpr std::array<std::uint16_t, 12> body{0xb672, 0x65d1, 0x2401, 0xe7ff, 0xbf00, 0xbf00,
                                               0xe7ff, 0xb662, 0x2601, 0xbf00, 0xbe00, 0xbf00};
  for(auto const live : {false, true})
  {
    CAPTURE(live);
    fixture f;
    core::machine_internals::set_budget_off(*f.target, live);
    f.model().raise_on_go = true;
    f.enter("store_raise");
    REQUIRE(f.target->mem().write_block(code, std::as_bytes(std::span{body})));
    f.set("pc", code | 1u);
    auto const before = f.counters().budget_runs;
    auto const slice =
        f.run({.max_instructions = icount{1000}, .wall_budget = std::chrono::seconds{2}});
    REQUIRE(slice.stopped.has_value());
    CHECK(slice.stopped->reason == stop_reason::user);
    CHECK(slice.executed.value > 0);
    CHECK((f.counters().budget_runs > before) == not live);
    CAPTURE(slice.executed.value);
    CHECK(f.reg("r5") == 1);
    CHECK(f.reg("r7") == 0);
  }
}

TEST_CASE("coverage, a masked pending IRQ and stepping run the live block hook")
{
  SUBCASE("coverage")
  {
    fixture f;
    f.target->enable_coverage({});
    f.enter("store_raise");
    auto const before = f.counters().budget_runs;
    std::ignore       = f.run_to_stop();
    CHECK(f.counters().budget_runs == before);
  }
  SUBCASE("a masked pending IRQ turns a budget run live at the access")
  {
    fixture f;
    f.model().raise_on_go = true;
    f.enter("masked_raise");
    auto const before = f.counters().budget_runs;
    auto const slice =
        f.run({.max_instructions = icount{1000}, .wall_budget = std::chrono::seconds{2}});
    REQUIRE(slice.stopped.has_value());
    CHECK(slice.stopped->reason == stop_reason::user);
    CHECK(slice.executed.value > 0);
    CHECK(f.counters().budget_runs > before);
    CHECK(f.reg("r5") == 1);
  }
}
