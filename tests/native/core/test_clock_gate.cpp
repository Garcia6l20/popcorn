#include <popcorn/core/errors.hpp>
#include <popcorn/core/intercept.hpp>
#include <popcorn/core/log.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/core/services.hpp>
#include <popcorn/core/snapshot.hpp>
#include <popcorn/core/trace.hpp>
#include <popcorn/periph/bus_reset.hpp>
#include <popcorn/periph/model.hpp>

#include "../../../native/src/core/machine_impl.hpp"
#include "../fake_services.hpp"
#include "../support/inert_model.hpp"
#include "../support/machine_fixture.hpp"
#include "../support/test_arch.hpp"
#include "../support/test_model.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace
{
namespace core = popcorn::core;
using popcorn::address;
using popcorn::initiator;
using popcorn::stop_reason;
using popcorn::testing::m33;
using popcorn::testing::sym;
using popcorn::testing::test_model;
using fixture = popcorn::testing::machine_fixture<>;

constexpr address testdev = 0x4001'0000;
constexpr address data    = testdev + test_model::data_offset;
constexpr address rw      = testdev + 0x08;

auto options(bool strict = false) -> core::machine_options
{
  core::machine_options made;
  made.models.emplace("TESTDEV", popcorn::testing::test_model_factory());
  made.strict_peripherals = strict;
  return made;
}

struct gated
{
  fixture                       f;
  std::vector<core::log_record> logged{};
  core::hook_handle             sink{};

  explicit gated(bool strict = false) : f{m33(), "mmio", options(strict)}
  {
    sink = f.target->add_log_sink([this](core::log_record const& record) {
      if(record.kind == core::log_kind::model_warning)
      {
        logged.push_back(record);
      }
    });
    REQUIRE(f.target->mem().write(data, 4, 5));
    REQUIRE(f.target->mem().write(rw, 4, 6));
    model().accesses.clear();
  }

  [[nodiscard]] auto bus() const -> core::machine_services&
  {
    return core::machine_internals::services(*f.target);
  }

  [[nodiscard]] auto model() const -> test_model&
  {
    return *static_cast<test_model*>(f.target->model("TESTDEV"));
  }

  [[nodiscard]] auto peek(address at) const -> std::uint32_t
  {
    return f.target->peripheral("TESTDEV").peek(at - testdev);
  }

  void gate(bool on) const
  {
    bus().gate_bus_clock("TESTDEV", on);
  }

  void loop(std::string_view entry, address target, std::uint32_t n) const
  {
    f.set("pc", sym("mmio", entry));
    f.set("r0", n);
    f.set("r2", target);
    auto const done = entry == "read_entry" ? "read_done" : "write_done";
    REQUIRE(f.run_to_stop({.until = sym("mmio", done)}).reason == stop_reason::address);
  }
};

auto mmio_records(core::machine& target) -> std::vector<core::trace_record>
{
  return target.trace().records()
       | std::views::filter([](auto const& r) { return r.kind == core::trace_kind::mmio; })
       | std::ranges::to<std::vector>();
}
}

TEST_CASE("a gated peripheral loses CPU and DMA writes and reads 0, the debugger is served")
{
  gated g;
  CHECK_FALSE(g.f.target->clock_gated("TESTDEV"));
  g.gate(true);
  CHECK(g.f.target->clock_gated("TESTDEV"));
  for(auto const by : {initiator::cpu, initiator::dma})
  {
    CAPTURE(by);
    CHECK(g.bus().write(data, 4, 7, by).has_value());
    CHECK(g.bus().write(rw, 2, 7, by).has_value());
    CHECK(g.bus().read(data, 4, by) == 0u);
    CHECK(g.bus().read(rw, 1, by) == 0u);
  }
  CHECK(g.model().accesses.empty());
  CHECK(g.peek(data) == 5);
  CHECK(g.peek(rw) == 6);

  CHECK(g.f.target->mem().read(data, 4) == 5u);
  CHECK(g.f.target->mem().read(rw, 4) == 6u);
  REQUIRE(g.f.target->mem().write(data, 4, 9));
  CHECK(g.peek(data) == 9);
  CHECK(g.model().accesses.size() == 2);

  g.gate(false);
  CHECK_FALSE(g.f.target->clock_gated("TESTDEV"));
  CHECK(g.bus().read(data, 4, initiator::dma) == 9u);
  CHECK(g.bus().write(rw, 4, 3, initiator::cpu).has_value());
  CHECK(g.peek(rw) == 3);
}

TEST_CASE("gated DMA block accesses read zeros and write nothing")
{
  gated g;
  g.gate(true);
  std::array<std::byte, 8> out{std::byte{0xaa}, std::byte{0xaa}, std::byte{0xaa}, std::byte{0xaa},
                               std::byte{0xaa}, std::byte{0xaa}, std::byte{0xaa}, std::byte{0xaa}};
  REQUIRE(g.bus().read_block(rw, out, initiator::dma));
  CHECK(std::ranges::all_of(out, [](std::byte b) { return b == std::byte{0}; }));
  std::array<std::byte, 4> const in{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
  REQUIRE(g.bus().write_block(data, in, initiator::dma));
  CHECK(g.peek(data) == 5);
  CHECK(g.model().accesses.empty());
  REQUIRE(g.f.target->mem().read_block(rw, out));
  CHECK(out[0] == std::byte{6});
}

TEST_CASE("the first gated access of the machine's life logs one warning naming the register")
{
  gated g;
  g.gate(true);
  auto const pc = static_cast<address>(g.f.reg("pc"));
  CHECK(g.bus().write(data, 4, 7, initiator::cpu).has_value());
  CHECK(g.bus().read(rw, 4, initiator::dma) == 0u);
  CHECK(g.bus().write(data, 4, 7, initiator::cpu).has_value());
  REQUIRE(g.logged.size() == 1);
  CHECK(g.logged[0].peripheral == "TESTDEV");
  CHECK(g.logged[0].reg == "DATA");
  CHECK(g.logged[0].offset == test_model::data_offset);
  CHECK(g.logged[0].write);
  CHECK(g.logged[0].pc == pc);
  CHECK(g.logged[0].message.contains("bus clock gated"));
  g.gate(false);
  g.gate(true);
  g.f.target->reset();
  g.gate(true);
  CHECK(g.bus().read(data, 4, initiator::cpu) == 0u);
  CHECK(g.logged.size() == 1);
}

TEST_CASE("a debugger access to a gated peripheral logs nothing")
{
  gated g;
  g.gate(true);
  CHECK(g.f.target->mem().read(data, 4) == 5u);
  REQUIRE(g.f.target->mem().write(rw, 4, 1));
  CHECK(g.logged.empty());
}

TEST_CASE("under strict_peripherals a gated CPU or DMA access is a model_error")
{
  gated g{true};
  g.gate(true);
  CHECK_THROWS_AS(std::ignore = g.bus().write(data, 4, 7, initiator::cpu), popcorn::model_error);
  CHECK_THROWS_AS(std::ignore = g.bus().read(rw, 4, initiator::dma), popcorn::model_error);
  CHECK_THROWS_AS(std::ignore = g.bus().write(rw, 4, 7, initiator::dma), popcorn::model_error);
  CHECK(g.peek(data) == 5);
  CHECK(g.peek(rw) == 6);
  CHECK(g.model().accesses.empty());
  CHECK(g.logged.size() == 1);
  CHECK(g.f.target->mem().read(data, 4) == 5u);
  REQUIRE(g.f.target->mem().write(rw, 4, 1));
  CHECK(g.peek(rw) == 1);
  g.gate(false);
  CHECK(g.bus().read(rw, 4, initiator::cpu) == 1u);
}

TEST_CASE("firmware loads and stores to a gated peripheral read 0, are lost and are traced")
{
  gated g;
  g.gate(true);
  g.f.target->trace().clear(g.f.target->virtual_clock().now());
  g.loop("write_entry", data, 3);
  CHECK(g.peek(data) == 5);
  g.loop("read_entry", data, 3);
  CHECK(g.f.reg("r1") == 0);
  CHECK(g.model().accesses.empty());
  auto const records = mmio_records(*g.f.target);
  REQUIRE(records.size() == 6);
  CHECK(records[0].payload[1] == data);
  CHECK(records[2].payload[2] == 2);
  CHECK(records[3].payload[2] == 0);
  CHECK(g.logged.size() == 1);

  g.gate(false);
  g.loop("read_entry", data, 3);
  CHECK(g.f.reg("r1") == 15);
  CHECK(g.model().accesses.size() == 3);
}

TEST_CASE("a machine reset ungates every peripheral")
{
  gated g;
  g.gate(true);
  g.f.target->reset();
  CHECK_FALSE(g.f.target->clock_gated("TESTDEV"));
  CHECK(g.bus().read(rw, 4, initiator::cpu) == 0u);
  REQUIRE(g.bus().write(rw, 4, 2, initiator::cpu));
  CHECK(g.peek(rw) == 2);
}

TEST_CASE("a snapshot keeps the gate, in the same machine and in another one")
{
  gated g;
  g.gate(true);
  auto const taken = g.f.target->snapshot();
  CHECK(taken.data().peripherals.front().clock_gated == 1);
  g.gate(false);
  g.f.target->restore(taken);
  CHECK(g.f.target->clock_gated("TESTDEV"));
  CHECK(g.bus().read(data, 4, initiator::cpu) == 0u);

  gated other;
  other.f.target->restore(
      core::snapshot{core::decode_snapshot(core::encode_snapshot(taken.data()))});
  CHECK(other.f.target->clock_gated("TESTDEV"));
  CHECK(other.bus().read(data, 4, initiator::cpu) == 0u);

  g.gate(false);
  auto const open = g.f.target->snapshot();
  g.gate(true);
  g.f.target->restore(open);
  CHECK_FALSE(g.f.target->clock_gated("TESTDEV"));
}

TEST_CASE("gating an unknown peripheral, or on services without peripherals, is a config_error")
{
  gated g;
  CHECK_THROWS_AS(g.bus().gate_bus_clock("NOPE", true), popcorn::config_error);
  CHECK_THROWS_AS(std::ignore = g.f.target->clock_gated("NOPE"), popcorn::config_error);
  popcorn::testing::fake_services services;
  CHECK_THROWS_AS(services.gate_bus_clock("TESTDEV", true), popcorn::config_error);
}

namespace
{
class gate_driver final : public popcorn::testing::inert_model
{
public:
  explicit gate_driver(popcorn::periph::model_context& ctx) : ctx_{ctx}
  {}

  void reset(popcorn::periph::register_file&, popcorn::reset_kind) override
  {
    ctx_.services.gate_bus_clock("UNMODELLED", true);
    if(machine != nullptr)
    {
      peer_stat = machine->peripheral("UNMODELLED").peek(4);
      peer_held = machine->held_in_reset("UNMODELLED");
    }
  }

  core::machine*               machine = nullptr;
  std::optional<std::uint32_t> peer_stat{};
  std::optional<bool>          peer_held{};

private:
  popcorn::periph::model_context& ctx_;
};

class held_listener final : public popcorn::testing::inert_model,
                            public popcorn::periph::bus_reset_listener
{
public:
  [[nodiscard]] auto claims() const -> std::vector<std::string> override
  {
    return {"DATA"};
  }
  void on_bus_reset() override
  {}
  void on_held_write(std::uint32_t, popcorn::periph::access_ctx) override
  {
    ++held_writes;
  }

  int held_writes = 0;
};
}

TEST_CASE("a model reset gates a peripheral placed after it, and the gate stays after the reset")
{
  core::machine_options made;
  made.models.emplace(
      "TESTDEV", [](popcorn::periph::model_context& ctx, popcorn::peripheral_desc const&) {
        return std::make_shared<gate_driver>(ctx);
      });
  fixture f{m33(), "mmio", std::move(made)};
  CHECK(f.target->clock_gated("UNMODELLED"));
  CHECK_FALSE(f.target->clock_gated("TESTDEV"));
  constexpr address unmodelled = 0x4002'0000;
  f.target->peripheral("UNMODELLED").poke(0, 3);
  auto& bus = core::machine_internals::services(*f.target);
  REQUIRE(bus.write(unmodelled, 4, 7, initiator::cpu));
  CHECK(f.target->peripheral("UNMODELLED").peek(0) == 3);
  bus.gate_bus_clock("UNMODELLED", false);
  auto& driver   = dynamic_cast<gate_driver&>(*f.target->model("TESTDEV"));
  driver.machine = f.target.get();
  bus.hold_in_reset("UNMODELLED", true);
  f.target->peripheral("UNMODELLED").poke(4, 5);
  f.target->reset();
  CHECK(f.target->clock_gated("UNMODELLED"));
  CHECK(driver.peer_stat == 1u);
  CHECK(driver.peer_held == false);
}

TEST_CASE("a watch on a gated register sees the lost writes and the reads as 0")
{
  gated                        g;
  std::vector<core::watch_hit> hits;
  auto const                   watch =
      g.f.target->add_watch(data, 4, core::watch_kind::access, [&hits](core::watch_hit const& hit) {
        hits.push_back(hit);
        return core::watch_action::resume;
      });
  g.gate(true);
  g.loop("write_entry", data, 2);
  g.loop("read_entry", data, 2);
  REQUIRE(hits.size() == 4);
  CHECK(hits[0].write);
  CHECK(hits[0].value == 0);
  CHECK(hits[1].write);
  CHECK(hits[1].value == 1);
  CHECK_FALSE(hits[2].write);
  CHECK(hits[2].value == 0);
  CHECK_FALSE(hits[3].write);
  CHECK(hits[3].value == 0);
  CHECK(g.peek(data) == 5);
}

TEST_CASE("a gated write to a peripheral held in reset reaches no on_held_write, inferred order")
{
  core::machine_options made;
  made.models.emplace(
      "TESTDEV", [](popcorn::periph::model_context&, popcorn::peripheral_desc const&) {
        return std::make_shared<held_listener>();
      });
  fixture f{m33(), "mmio", std::move(made)};
  auto&   bus      = core::machine_internals::services(*f.target);
  auto&   listener = dynamic_cast<held_listener&>(*f.target->model("TESTDEV"));
  bus.hold_in_reset("TESTDEV", true);
  REQUIRE(bus.write(data, 4, 1, initiator::cpu));
  CHECK(listener.held_writes == 1);
  bus.gate_bus_clock("TESTDEV", true);
  REQUIRE(bus.write(data, 4, 1, initiator::cpu));
  REQUIRE(bus.write(data, 4, 1, initiator::dma));
  CHECK(listener.held_writes == 1);
  CHECK(bus.read(testdev, 4, initiator::cpu) == 0u);
  bus.gate_bus_clock("TESTDEV", false);
  CHECK(bus.read(testdev, 4, initiator::cpu) == 0x1234'5678u);
}

TEST_CASE("under strict_peripherals a firmware store to a gated peripheral ends the run")
{
  gated g{true};
  g.gate(true);
  g.f.set("pc", sym("mmio", "write_entry"));
  g.f.set("r0", 2);
  g.f.set("r2", data);
  CHECK_THROWS_AS(std::ignore = g.f.run_to_stop(), popcorn::model_error);
  CHECK(g.peek(data) == 5);
  CHECK(g.model().accesses.empty());
}
