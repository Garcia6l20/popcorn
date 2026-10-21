#include <popcorn/core/errors.hpp>
#include <popcorn/core/intercept.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/core/services.hpp>
#include <popcorn/core/snapshot.hpp>
#include <popcorn/periph/snapshot_listener.hpp>

#include "../support/machine_fixture.hpp"
#include "../support/test_arch.hpp"
#include "../support/test_model.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
namespace core = popcorn::core;
namespace arch = popcorn::arch;
using popcorn::address;
using popcorn::icount;
using popcorn::initiator;
using popcorn::part_description;
using popcorn::stop_reason;
using popcorn::testing::m33;
using popcorn::testing::sym;
using popcorn::testing::test_model;
using fixture = popcorn::testing::machine_fixture<>;

constexpr address       sram          = 0x2000'0000;
constexpr std::uint32_t sram_size     = 0xc'0000;
constexpr address       sram_code     = 0x0a00'0000;
constexpr address       flash         = 0x0800'0000;
constexpr address       unmapped_page = 0x3000'0000;
constexpr address       wonce         = 0x4001'0010;

auto with_model() -> core::machine_options
{
  core::machine_options options;
  options.models.emplace("TESTDEV", popcorn::testing::test_model_factory());
  return options;
}

auto booted(
    std::string_view      firmware = "snapshot",
    core::machine_options options  = with_model(),
    part_description      desc     = m33()) -> fixture
{
  fixture f{std::move(desc), firmware, std::move(options)};
  REQUIRE(f.run_to_stop().reason == stop_reason::user);
  return f;
}

auto fn(std::string_view name, std::string_view firmware = "snapshot") -> address
{
  return sym(firmware, name);
}

auto model(auto const& f) -> test_model&
{
  return dynamic_cast<test_model&>(*f.target->model("TESTDEV"));
}

struct observed
{
  std::vector<std::uint64_t> registers{};
  std::uint64_t              sram_hash{};
  icount                     now{};

  friend auto operator==(observed const&, observed const&) -> bool = default;
};

auto observe(auto const& f) -> observed
{
  observed seen{.now = f.target->virtual_clock().now()};
  for(auto const& reg : f.module->registers())
  {
    seen.registers.push_back(f.reg(reg.name));
  }
  std::vector<std::byte> bytes(sram_size);
  REQUIRE(f.target->mem().read_block(sram, bytes));
  std::uint64_t hash = 0xcbf2'9ce4'8422'2325;
  for(auto const byte : bytes)
  {
    hash = (hash ^ std::to_integer<std::uint64_t>(byte)) * 0x100'0000'01b3;
  }
  seen.sram_hash = hash;
  return seen;
}

auto run_million(auto const& f) -> observed
{
  core::slice_request request{.exact_instructions = icount{1'000'000}};
  request.max_instructions = icount{2'000'000};
  request.wall_budget      = std::chrono::seconds{60};
  auto const result        = f.run(request);
  CHECK(result.executed == icount{1'000'000});
  return observe(f);
}

auto call(fixture const& f, address entry) -> std::uint64_t
{
  auto const result = f.target->call(
      {.entry           = entry,
       .returns         = arch::value_kind::i32,
       .abi             = arch::float_abi::soft,
       .return_sentinel = unmapped_page},
      icount{10'000});
  REQUIRE(result.stopped.reason == stop_reason::returned);
  return result.value;
}

struct unreset
{
  popcorn::testing::test_arch*   module = nullptr;
  std::unique_ptr<core::machine> target;

  unreset()
  {
    target = std::make_unique<core::machine>(
        m33(),
        [this](popcorn::core_desc const& unit) -> std::unique_ptr<arch::arch> {
          auto made = std::make_unique<popcorn::testing::test_arch>(unit);
          module    = made.get();
          return made;
        },
        with_model());
    target->load(popcorn::testing::elf("snapshot"));
  }

  auto write(address at, std::uint32_t value) const
  {
    return module->attached_services().write(at, 4, value, initiator::cpu);
  }

  auto word(address at) const -> std::uint32_t
  {
    return *target->mem().read(at, 4);
  }
};

auto reread(core::snapshot const& taken) -> core::snapshot
{
  return core::snapshot{core::decode_snapshot(core::encode_snapshot(taken.data()))};
}

void write_half(fixture const& f, address at, std::uint16_t value)
{
  REQUIRE(f.target->mem().write(at, 2, value));
}
}

TEST_CASE("a restored machine replays the same run, again and again and in another machine")
{
  auto       f     = booted();
  auto const taken = f.target->snapshot();
  auto const live  = run_million(f);
  CHECK(f.module->timer_expiries > 900);
  CHECK(model(f).fired_at.has_value());
  f.target->restore(taken);
  CHECK(run_million(f) == live);
  for(int round = 1; round <= 1000; ++round)
  {
    f.target->restore(taken);
    if(round % 100 == 0)
    {
      CAPTURE(round);
      CHECK(run_million(f) == live);
    }
  }
  auto other = fixture{m33(), "snapshot", with_model()};
  other.target->restore(core::snapshot{core::decode_snapshot(core::encode_snapshot(taken.data()))});
  CHECK(run_million(other) == live);
}

TEST_CASE("snapshot and restore refuse slices, handlers and calls")
{
  auto       f     = booted();
  auto const taken = f.target->snapshot();
  SUBCASE("from a handler")
  {
    auto const hook =
        f.target->add_watch(fn("ticks"), 4, core::watch_kind::write, [&](core::watch_hit const&) {
          CHECK_THROWS_AS(std::ignore = f.target->snapshot(), popcorn::machine_running);
          CHECK_THROWS_AS(f.target->restore(taken), popcorn::machine_running);
          return core::watch_action::stop;
        });
    CHECK(f.run_to_stop().reason == stop_reason::watchpoint);
  }
  SUBCASE("during a call")
  {
    f.target->begin_call(
        {.entry = fn("dirty"), .abi = arch::float_abi::soft, .return_sentinel = unmapped_page});
    CHECK_THROWS_AS(std::ignore = f.target->snapshot(), popcorn::machine_busy);
    CHECK_THROWS_AS(f.target->restore(taken), popcorn::machine_busy);
    std::ignore = f.target->end_call();
  }
}

TEST_CASE("a model load that throws leaves the machine invalid until a restore succeeds")
{
  auto       f        = booted();
  auto const taken    = f.target->snapshot();
  model(f).throw_load = true;
  CHECK_THROWS_AS(f.target->restore(taken), std::runtime_error);
  CHECK_THROWS_AS(std::ignore = f.run(), popcorn::emulation_error);
  CHECK_THROWS_AS(std::ignore = f.target->snapshot(), popcorn::emulation_error);
  model(f).throw_load = false;
  f.target->restore(taken);
  CHECK_NOTHROW(std::ignore = f.run());
}

TEST_CASE("snapshots from other machines and other inputs are refused")
{
  auto       a     = booted();
  auto       b     = booted();
  auto const taken = a.target->snapshot();
  CHECK_THROWS_AS(b.target->restore(taken), popcorn::emulation_error);
  auto const bytes = core::encode_snapshot(taken.data());
  CHECK_NOTHROW(b.target->restore(core::snapshot{core::decode_snapshot(bytes)}));
  auto other = fixture{m33(), "intercepts", with_model()};
  try
  {
    other.target->restore(core::snapshot{core::decode_snapshot(bytes)});
    FAIL("restored a snapshot of another image");
  }
  catch(popcorn::config_error const& refused)
  {
    CHECK(refused.key() == "image_digests");
  }
}

TEST_CASE("every restore bumps the generation by one")
{
  auto       f      = booted();
  auto const taken  = f.target->snapshot();
  auto const before = f.target->generation();
  f.target->restore(taken);
  CHECK(f.target->generation() == before + 1);
  f.target->restore(taken);
  CHECK(f.target->generation() == before + 2);
}

TEST_CASE("restored SRAM code runs, not the code it replaced")
{
  auto       f     = booted();
  auto const entry = fn("ram_fn") | 1;
  CHECK(call(f, entry) == 1);
  auto const taken = f.target->snapshot();
  write_half(f, fn("ram_fn"), 0x2002);
  CHECK(call(f, entry) == 2);
  f.target->restore(taken);
  CHECK(call(f, entry) == 1);
}

TEST_CASE("restored flash code runs, whatever rewrote the flash")
{
  auto       f       = booted("basic", {});
  auto const patched = fn("patched", "basic");
  auto const taken   = f.target->snapshot();
  auto const r0      = f.reg("r0");
  SUBCASE("through mem()")
  {
    write_half(f, patched, 0x2002);
  }
  SUBCASE("through load()")
  {
    popcorn::image::image patch;
    patch.segments.push_back({
        .name        = "patch",
        .start       = patched,
        .bytes       = {std::byte{0x02}, std::byte{0x20}},
        .memory_size = 2,
        .executable  = true,
        .writable    = false
    });
    f.target->load(patch);
  }
  CHECK(call(f, patched | 1) == 2);
  CHECK(call(f, (patched - flash) | 1) == 2);
  f.target->restore(taken);
  CHECK(f.reg("r0") == r0);
  CHECK(call(f, patched | 1) == 1);
  write_half(f, patched, 0x2002);
  CHECK(call(f, (patched - flash) | 1) == 2);
  f.target->restore(taken);
  CHECK(call(f, (patched - flash) | 1) == 1);
}

TEST_CASE("restore removes code at every mapping of an aliased buffer")
{
  auto       f             = booted("snapshot", with_model(), popcorn::testing::m33_code_alias());
  auto const through_alias = (fn("ram_fn") - sram + sram_code) | 1;
  CHECK(call(f, through_alias) == 1);
  auto const taken = f.target->snapshot();
  write_half(f, fn("ram_fn"), 0x2002);
  CHECK(call(f, through_alias) == 2);
  f.target->restore(taken);
  CHECK(call(f, through_alias) == 1);
}

TEST_CASE("a restore that changed only data keeps the translated code")
{
  auto       f     = booted();
  auto const taken = f.target->snapshot();
  auto const timed = [&](bool dirty_data) {
    auto best = std::chrono::nanoseconds::max();
    for(int round = 0; round < 5; ++round)
    {
      if(dirty_data)
      {
        std::vector<std::byte> junk(0x1'0000, std::byte{0x5a});
        REQUIRE(f.target->mem().write_block(fn("scratch"), junk));
      }
      f.target->restore(taken);
      auto const start = std::chrono::steady_clock::now();
      std::ignore      = run_million(f);
      best             = std::min(best, std::chrono::steady_clock::now() - start);
    }
    return best;
  };
  auto const warm  = timed(false);
  auto const dirty = timed(true);
  CAPTURE(warm.count());
  CAPTURE(dirty.count());
  CHECK(dirty < 2 * warm + std::chrono::microseconds{500});
}

TEST_CASE("write-once locks survive a reset and travel through bytes")
{
  auto  f        = booted();
  auto& services = f.module->attached_services();
  REQUIRE(services.write(wonce, 4, 0x11, initiator::cpu));
  REQUIRE(services.write(wonce, 4, 0x22, initiator::cpu));
  REQUIRE(f.word(wonce) == 0x11);
  auto const taken = f.target->snapshot();
  f.target->reset();
  f.target->restore(taken);
  REQUIRE(services.write(wonce, 4, 0x33, initiator::cpu));
  CHECK(f.word(wonce) == 0x11);
  auto other = fixture{m33(), "snapshot", with_model()};
  other.target->restore(core::snapshot{core::decode_snapshot(core::encode_snapshot(taken.data()))});
  REQUIRE(other.module->attached_services().write(wonce, 4, 0x44, initiator::cpu));
  CHECK(other.word(wonce) == 0x11);
}

TEST_CASE("restore rebuilds the wired-OR count of every active line")
{
  struct line_recorder : popcorn::testing::test_arch
  {
    using test_arch::test_arch;
    std::vector<std::pair<std::uint32_t, bool>> lines{};
    void set_irq_line(std::uint32_t source, bool active) override
    {
      lines.emplace_back(source, active);
      test_arch::set_irq_line(source, active);
    }
  };
  popcorn::testing::machine_fixture<line_recorder> f{m33(), "snapshot", with_model()};
  REQUIRE(f.run_to_stop().reason == stop_reason::user);
  auto& line = model(f).context().irqs.front();
  line.raise();
  auto const taken = f.target->snapshot();
  f.target->reset();
  f.module->lines.clear();
  f.target->restore(taken);
  CHECK(line.level());
  line.lower();
  CHECK(
      f.module->lines
      == std::vector<std::pair<std::uint32_t, bool>>{
          {3, false}
  });
}

TEST_CASE("restore drops return frames and the intercept resume point")
{
  fixture f{m33(), "intercepts", {}};
  REQUIRE(f.run_to_stop().reason == stop_reason::reset_request);
  std::vector<core::returned_call> returns;
  int                              stops = 0;
  auto const                       hook  = f.target->add_return_hook(
      fn("seven", "intercepts"), {}, arch::value_kind::i32,
      [&](core::returned_call const& r) { returns.push_back(r); });
  auto stopper =
      f.target->add_intercept(fn("seven", "intercepts"), [&](core::intercept_call const&) {
        ++stops;
        return core::intercept_result::stop(stop_reason::user);
      });
  f.set("pc", fn("call_seven", "intercepts"));
  f.set("lr", fn("halt", "intercepts") | 1);
  REQUIRE(f.run_to_stop().where.pc == fn("seven", "intercepts"));
  auto const taken = f.target->snapshot();
  REQUIRE(taken.data().return_hooks.size() == 1);
  CHECK(taken.data().return_hooks[0].in_flight.size() == 1);
  CHECK(taken.data().intercepts.at(0).calls == 1);
  f.target->restore(taken);
  CHECK(f.run_to_stop().where.pc == fn("seven", "intercepts"));
  CHECK(stops == 2);
  f.target->restore(taken);
  stopper.reset();
  auto const end = f.run_to_stop();
  CHECK(end.where.pc == fn("halt", "intercepts") + 2);
  REQUIRE(returns.size() == 1);
  CHECK(returns[0].value == 7);
}

TEST_CASE("snapshots outlive their machine")
{
  std::vector<core::snapshot> kept;
  {
    auto f = booted();
    for(int round = 0; round < 10'000; ++round)
    {
      kept.push_back(f.target->snapshot());
      f.target->restore(kept.front());
      if(kept.size() > 4)
      {
        kept.erase(kept.begin() + 1);
      }
    }
  }
  CHECK(kept.size() == 4);
}

TEST_CASE("write-once locks restore whether or not either machine was ever reset")
{
  SUBCASE("a serialized snapshot into a machine never reset")
  {
    unreset a;
    REQUIRE(a.write(wonce, 0x11));
    unreset b;
    b.target->restore(reread(a.target->snapshot()));
    REQUIRE(b.write(wonce, 0x22));
    CHECK(b.word(wonce) == 0x11);
  }
  SUBCASE("an own snapshot taken before any reset, restored after one")
  {
    unreset    a;
    auto const taken = a.target->snapshot();
    CHECK(taken.data().peripherals.front().write_locked.empty());
    a.target->reset();
    REQUIRE(a.write(wonce, 0x11));
    a.target->restore(taken);
    REQUIRE(a.write(wonce, 0x22));
    CHECK(a.word(wonce) == 0x22);
    REQUIRE(a.write(wonce, 0x33));
    CHECK(a.word(wonce) == 0x22);
  }
}

TEST_CASE("a serialized snapshot with a foreign event owner or clock domain is refused up front")
{
  auto f      = booted();
  std::ignore = model(f).context().clock.domain("apb");
  auto data   = f.target->snapshot().data();
  SUBCASE("event owner")
  {
    REQUIRE_FALSE(data.events.pending.empty());
    data.events.pending.front().id.owner = 2;
  }
  SUBCASE("clock domain")
  {
    REQUIRE_FALSE(data.clock.domain_names.empty());
    data.clock.domain_names.front() = "ahb";
  }
  auto other  = fixture{m33(), "snapshot", with_model()};
  std::ignore = model(other).context().clock.domain("apb");
  CHECK_THROWS_AS(other.target->restore(core::snapshot{data}), popcorn::config_error);
  CHECK_NOTHROW(std::ignore = other.target->snapshot());
  CHECK_NOTHROW(std::ignore = other.run());
}

namespace
{
class storage_model final : public popcorn::periph::model
{
public:
  [[nodiscard]] auto claims() const -> std::vector<std::string> override
  {
    return {};
  }

  void reset(popcorn::periph::register_file&, popcorn::reset_kind) override
  {}

  auto on_read(
      popcorn::periph::register_file&,
      std::uint32_t,
      std::uint8_t,
      popcorn::periph::access_ctx) -> std::expected<std::uint32_t, popcorn::bus_error> override
  {
    return 0;
  }

  auto on_write(
      popcorn::periph::register_file&,
      std::uint32_t,
      std::uint8_t,
      std::uint32_t,
      std::uint32_t,
      popcorn::periph::access_ctx) -> std::expected<void, popcorn::bus_error> override
  {
    return {};
  }

  void on_event(std::uint32_t, std::uint64_t) override
  {}

  void save(std::vector<std::byte>&) const override
  {
    ++saves;
  }

  void load(std::span<std::byte const>) override
  {
    ++loads;
  }

  [[nodiscard]] auto stateless() const -> bool override
  {
    return true;
  }

  mutable int saves{};
  int         loads{};
};

auto with_storage_model() -> core::machine_options
{
  core::machine_options options;
  options.models.emplace(
      "TESTDEV", [](popcorn::periph::model_context&, popcorn::peripheral_desc const&) {
        return std::make_shared<storage_model>();
      });
  return options;
}

auto without_model_bytes(core::snapshot_data data) -> core::snapshot
{
  auto const removed = std::erase_if(
      data.models, [](core::snapshot_part const& part) { return part.owner == "TESTDEV"; });
  REQUIRE(removed == 1);
  return core::snapshot{std::move(data)};
}
}

TEST_CASE("a snapshot holds no bytes for a stateless model, restored from its register storage")
{
  auto f = fixture{m33(), "snapshot", with_storage_model()};
  f.target->peripheral("TESTDEV").poke(test_model::data_offset, 0x1234'5678);
  auto const taken = f.target->snapshot();
  CHECK(std::ranges::none_of(taken.data().models, [](core::snapshot_part const& part) {
    return part.owner == "TESTDEV";
  }));
  f.target->peripheral("TESTDEV").poke(test_model::data_offset, 0);
  REQUIRE_NOTHROW(f.target->restore(taken));
  CHECK(f.target->peripheral("TESTDEV").peek(test_model::data_offset) == 0x1234'5678);
  auto const& made = dynamic_cast<storage_model&>(*f.target->model("TESTDEV"));
  CHECK(made.saves == 0);
  CHECK(made.loads == 0);
  CHECK_NOTHROW(std::ignore = f.target->snapshot());
}

TEST_CASE("a save that carries bytes for a stateless model restores without loading them")
{
  auto       source = booted();
  auto const taken  = source.target->snapshot();
  REQUIRE(std::ranges::any_of(taken.data().models, [](core::snapshot_part const& part) {
    return part.owner == "TESTDEV";
  }));
  auto f = fixture{m33(), "snapshot", with_storage_model()};
  REQUIRE_NOTHROW(f.target->restore(core::snapshot{taken.data()}));
  auto const& made = dynamic_cast<storage_model&>(*f.target->model("TESTDEV"));
  CHECK(made.loads == 0);
  CHECK_NOTHROW(std::ignore = f.target->snapshot());
}

TEST_CASE("a save with no bytes for a model with state is refused")
{
  auto       f     = booted();
  auto const taken = without_model_bytes(f.target->snapshot().data());
  try
  {
    f.target->restore(taken);
    FAIL("restored a model with state from storage alone");
  }
  catch(popcorn::config_error const& refused)
  {
    CHECK(refused.key() == "models.TESTDEV");
  }
  CHECK_NOTHROW(std::ignore = f.target->snapshot());
}

namespace
{
class settling_model final : public popcorn::periph::model,
                             public popcorn::periph::snapshot_listener
{
public:
  explicit settling_model(popcorn::periph::model_context& ctx) noexcept : context{ctx}
  {}

  [[nodiscard]] auto claims() const -> std::vector<std::string> override
  {
    return {};
  }

  void reset(popcorn::periph::register_file&, popcorn::reset_kind) override
  {}

  auto on_read(
      popcorn::periph::register_file&,
      std::uint32_t,
      std::uint8_t,
      popcorn::periph::access_ctx) -> std::expected<std::uint32_t, popcorn::bus_error> override
  {
    return 0;
  }

  auto on_write(
      popcorn::periph::register_file&,
      std::uint32_t,
      std::uint8_t,
      std::uint32_t,
      std::uint32_t,
      popcorn::periph::access_ctx) -> std::expected<void, popcorn::bus_error> override
  {
    return {};
  }

  void on_event(std::uint32_t, std::uint64_t) override
  {}

  void before_snapshot() override
  {
    if(throws)
    {
      throw popcorn::model_error("TESTDEV", "SETTLE", 0);
    }
    ++settled;
    context.regs.poke(
        popcorn::testing::test_model::data_offset, static_cast<std::uint32_t>(settled));
    context.irqs.front().raise();
  }

  void save(std::vector<std::byte>& out) const override
  {
    out.push_back(static_cast<std::byte>(settled));
  }

  void load(std::span<std::byte const> in) override
  {
    settled = static_cast<int>(in.front());
  }

  popcorn::periph::model_context& context;
  int                             settled{};
  bool                            throws{};
};

auto with_settling_model() -> core::machine_options
{
  core::machine_options options;
  options.models.emplace(
      "TESTDEV", [](popcorn::periph::model_context& ctx, popcorn::peripheral_desc const&) {
        return std::make_shared<settling_model>(ctx);
      });
  return options;
}

auto saved_byte(core::snapshot const& taken) -> std::byte
{
  auto const found =
      std::ranges::find(taken.data().models, std::string{"TESTDEV"}, &core::snapshot_part::owner);
  REQUIRE(found != taken.data().models.end());
  REQUIRE(found->data.size() == 1);
  return found->data.front();
}

auto testdev_image(core::snapshot const& taken) -> core::peripheral_image const&
{
  auto const found = std::ranges::find(
      taken.data().peripherals, std::string{"TESTDEV"}, &core::peripheral_image::peripheral);
  REQUIRE(found != taken.data().peripherals.end());
  return *found;
}
}

TEST_CASE("a snapshot listener settles before anything is saved, a throw captures nothing")
{
  auto  f    = fixture{m33(), "snapshot", with_settling_model()};
  auto& made = dynamic_cast<settling_model&>(*f.target->model("TESTDEV"));
  REQUIRE_FALSE(made.context.irqs.front().level());
  auto const first = f.target->snapshot();
  CHECK(saved_byte(first) == std::byte{1});
  CHECK(testdev_image(first).words.at(popcorn::testing::test_model::data_offset / 4) == 1);
  CHECK(testdev_image(first).irq_levels.at(0) == 1);
  CHECK(saved_byte(f.target->snapshot()) == std::byte{2});
  made.throws = true;
  CHECK_THROWS_AS(std::ignore = f.target->snapshot(), popcorn::model_error);
  CHECK(made.settled == 2);
  made.throws = false;
  CHECK(saved_byte(f.target->snapshot()) == std::byte{3});
}
