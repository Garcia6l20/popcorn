#include <popcorn/arch/arch.hpp>

#include <popcorn/core/clock.hpp>
#include <popcorn/core/errors.hpp>
#include <popcorn/core/scheduler.hpp>
#include <popcorn/core/services.hpp>

#include "../fake_services.hpp"

#include <doctest/doctest.h>
#include <reflex/serde/bson.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <format>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace
{
namespace bson = reflex::serde::bson;

class toy_arch final : public popcorn::arch::arch
{
public:
  struct state
  {
    std::uint32_t                pending{};
    std::uint32_t                enabled{};
    std::uint32_t                active{};
    bool                         masked{};
    bool                         sleeping{};
    std::uint32_t                pc{};
    std::uint32_t                handler{};
    std::array<std::uint64_t, 4> args{};
    std::uint32_t                timer_fires{};
    std::uint32_t                link{};
  };

  static constexpr std::uint32_t timer_kind   = 1;
  static constexpr std::uint32_t timer_source = 7;
  static constexpr std::uint32_t svc_intno    = 2;

  void attach(popcorn::arch::arch_context const& ctx) override
  {
    if(ctx.description.core.cpu != "toy")
    {
      throw popcorn::config_error("toy", 0, "core.cpu", "toy");
    }
    context.emplace(ctx);
  }

  void reset(popcorn::address reset_vector) override
  {
    current         = {};
    current.pc      = reset_vector;
    current.enabled = ~std::uint32_t{};
    set_pending(false);
    context->events.schedule(popcorn::icount{100}, timer_kind, 0);
  }

  auto deliver_pending() -> popcorn::arch::trap_result override
  {
    std::uint32_t const ready = current.pending & current.enabled;
    if(ready != 0)
    {
      current.sleeping = false;
    }
    if(ready == 0 or current.masked)
    {
      return popcorn::arch::trap_result::resume();
    }
    auto const source = static_cast<std::uint32_t>(std::countr_zero(ready));
    auto const vector = context->services.read(0x100 + 4 * source, 4, popcorn::initiator::cpu);
    if(not vector)
    {
      return popcorn::arch::trap_result::fault(
          {popcorn::fault_kind::bus, 0x100 + 4 * source, 0, 0});
    }
    current.pending &= ~(std::uint32_t{1} << source);
    current.active |= std::uint32_t{1} << source;
    current.pc = *vector;
    refresh();
    return popcorn::arch::trap_result::resume();
  }

  [[nodiscard]] auto deliverable_now() const -> bool override
  {
    return not current.masked;
  }

  void set_irq_line(std::uint32_t source, bool active) override
  {
    if(active)
    {
      current.pending |= std::uint32_t{1} << source;
    }
    refresh();
  }

  auto on_interrupt(std::uint32_t intno) -> popcorn::arch::trap_result override
  {
    if(intno == svc_intno)
    {
      current.pc = current.handler;
      return popcorn::arch::trap_result::resume();
    }
    return popcorn::arch::trap_result::stop(popcorn::stop_reason::user);
  }

  auto on_invalid_insn() -> popcorn::arch::trap_result override
  {
    return popcorn::arch::trap_result::fault({popcorn::fault_kind::invalid, current.pc, 0, 0});
  }

  auto on_unmapped(popcorn::address addr, popcorn::arch::memory_op op)
      -> popcorn::arch::trap_result override
  {
    auto const kind = op == popcorn::arch::memory_op::fetch ? popcorn::fault_kind::invalid
                                                            : popcorn::fault_kind::unmapped;
    return popcorn::arch::trap_result::fault({kind, addr, 0, 0});
  }

  void on_event(std::uint32_t kind, std::uint64_t) override
  {
    if(kind == timer_kind)
    {
      ++current.timer_fires;
      set_irq_line(timer_source, true);
    }
  }

  [[nodiscard]] auto idle() const -> bool override
  {
    return current.sleeping;
  }

  [[nodiscard]] auto pc() const -> popcorn::address override
  {
    return current.pc;
  }

  [[nodiscard]] auto resume_address() const -> popcorn::address override
  {
    return current.pc | 1;
  }

  [[nodiscard]] auto registers() const -> std::span<popcorn::arch::cpu_register const> override
  {
    return core_registers;
  }

  void prepare_call(popcorn::arch::call_frame const& frame) override
  {
    if(frame.abi
       == popcorn::arch::float_abi::hard_single
       or frame.args.size()
       > current.args.size())
    {
      throw popcorn::config_error("toy", 0, "abi", "soft or hard_double, at most 4 arguments");
    }
    std::ranges::copy(
        frame.args | std::views::transform(&popcorn::arch::call_arg::bits), current.args.begin());
    current.pc     = frame.entry;
    current.masked = not frame.interrupts;
  }

  [[nodiscard]] auto call_result(popcorn::arch::call_frame const& frame) const
      -> std::uint64_t override
  {
    return frame.returns == popcorn::arch::value_kind::i32 ? current.args[0] & 0xffff'ffff
                                                           : current.args[0];
  }

  void read_args(popcorn::arch::call_frame& frame) const override
  {
    for(auto&& [arg, bits] : std::views::zip(frame.args, current.args))
    {
      arg.bits = bits;
    }
  }

  [[nodiscard]] auto return_address() const -> popcorn::address override
  {
    return current.link & ~popcorn::address{1};
  }

  void return_to_caller(
      std::optional<popcorn::arch::call_arg> const& value,
      popcorn::arch::float_abi) override
  {
    if(value)
    {
      current.args[0] = value->bits;
    }
    current.pc = return_address();
  }

  void save(std::vector<std::byte>& out) const override
  {
    bson::serializer{out}.dump(current);
  }

  void load(std::span<std::byte const> in) override
  {
    current = bson::deserializer{in}.load<state>();
    refresh();
  }

  void wfi_hook()
  {
    current.sleeping = true;
    context->services.end_slice();
  }

  void reset_register_write()
  {
    context->services.request_reset(popcorn::reset_kind::software);
  }

  static constexpr std::array core_registers{
      popcorn::arch::cpu_register{"pc", 11, 32},
      popcorn::arch::cpu_register{"sp", 12, 32},
  };

  state                                      current;
  std::optional<popcorn::arch::arch_context> context;

private:
  void refresh() noexcept
  {
    set_pending((current.pending & current.enabled) != 0);
  }
};

auto described(std::string cpu) -> popcorn::mcu_description
{
  popcorn::mcu_description description;
  description.core.cpu = std::move(cpu);
  return description;
}

struct rig
{
  popcorn::mcu_description        description = described("toy");
  popcorn::core::clock            clock{1'000'000};
  popcorn::core::scheduler        queue;
  popcorn::core::event_port       events{queue, popcorn::arch::arch_event_owner};
  popcorn::testing::fake_services services;
  std::unique_ptr<toy_arch>       module = std::make_unique<toy_arch>();

  rig()
  {
    module->attach({nullptr, description, clock, events, services});
    module->reset(0x0800'0000);
  }
};
}

static_assert(std::is_abstract_v<popcorn::arch::arch>);
static_assert(not std::is_copy_constructible_v<popcorn::arch::arch>);
static_assert(not std::is_move_constructible_v<popcorn::arch::arch>);
static_assert(std::is_trivially_copyable_v<popcorn::arch::call_arg>);

TEST_CASE("a dummy arch links and is created through a factory")
{
  popcorn::arch::arch_factory const factory =
      [](popcorn::core_desc const&) -> std::unique_ptr<popcorn::arch::arch> {
    return std::make_unique<toy_arch>();
  };
  auto const module = factory(described("toy").core);
  REQUIRE(module != nullptr);
  CHECK_FALSE(module->pending());

  popcorn::mcu_description        other = described("cortex-m33");
  popcorn::core::clock            clock{1'000'000};
  popcorn::core::scheduler        queue;
  popcorn::core::event_port       events{queue, popcorn::arch::arch_event_owner};
  popcorn::testing::fake_services services;
  CHECK_THROWS_AS(module->attach({nullptr, other, clock, events, services}), popcorn::config_error);
}

TEST_CASE("the arch owns its timer events and pends from them")
{
  rig setup;
  REQUIRE(setup.queue.pending().size() == 1);
  auto const timer = setup.queue.pending().front();
  CHECK(timer.id.owner == popcorn::arch::arch_event_owner);
  CHECK(timer.at == popcorn::icount{100});

  setup.services.poke_word(0x100 + 4 * toy_arch::timer_source, 0x0800'0400);
  setup.module->on_event(timer.id.kind, timer.payload);
  CHECK(setup.module->pending());
  CHECK(setup.module->deliverable_now());
  CHECK(setup.module->deliver_pending().resumes());
  CHECK(setup.module->pc() == 0x0800'0400);
  CHECK(setup.module->resume_address() == 0x0800'0401);
  CHECK_FALSE(setup.module->pending());
}

TEST_CASE("masked interrupts wake an idle core without entry")
{
  rig setup;
  setup.module->current.masked   = true;
  setup.module->current.sleeping = true;
  setup.module->set_irq_line(3, true);
  setup.module->set_irq_line(3, false);
  CHECK(setup.module->pending());
  CHECK_FALSE(setup.module->deliverable_now());
  CHECK(setup.module->deliver_pending().resumes());
  CHECK_FALSE(setup.module->idle());
  CHECK(setup.module->pc() == 0x0800'0000);
}

TEST_CASE("traps resume or stop with a fault")
{
  rig setup;
  setup.module->current.handler = 0x0800'0200;
  CHECK(setup.module->on_interrupt(toy_arch::svc_intno).resumes());
  CHECK(setup.module->pc() == 0x0800'0200);

  auto const bkpt = setup.module->on_interrupt(7);
  CHECK_FALSE(bkpt.resumes());
  CHECK(bkpt.reason() == popcorn::stop_reason::user);
  CHECK_FALSE(bkpt.fault_detail().has_value());

  auto const invalid = setup.module->on_invalid_insn();
  CHECK(invalid.reason() == popcorn::stop_reason::fault);
  REQUIRE(invalid.fault_detail().has_value());
  CHECK(invalid.fault_detail()->kind == popcorn::fault_kind::invalid);

  auto const unmapped = setup.module->on_unmapped(0x6000'0000, popcorn::arch::memory_op::write);
  REQUIRE(unmapped.fault_detail().has_value());
  CHECK(unmapped.fault_detail()->fault_address == 0x6000'0000);
  CHECK(unmapped.fault_detail()->kind == popcorn::fault_kind::unmapped);

  setup.module->set_irq_line(5, true);
  auto const entry = setup.module->deliver_pending();
  CHECK(entry.reason() == popcorn::stop_reason::fault);
  REQUIRE(entry.fault_detail().has_value());
  CHECK(entry.fault_detail()->kind == popcorn::fault_kind::bus);

  constexpr auto resumed = popcorn::arch::trap_result::resume();
  static_assert(resumed.resumes() and not resumed.reason() and not resumed.fault_detail());
}

TEST_CASE("a call frame is placed and its result read back")
{
  rig setup;
  setup.module->set_irq_line(2, true);
  std::vector<std::byte> interrupted;
  setup.module->save(interrupted);

  popcorn::arch::call_frame const frame{
      .entry = 0x0800'1000,
      .args =
          {{.kind = popcorn::arch::value_kind::i64, .bits = 0x1'0000'0002},
                 {.kind = popcorn::arch::value_kind::f32, .bits = std::bit_cast<std::uint32_t>(1.5f)}},
      .returns         = popcorn::arch::value_kind::i32,
      .abi             = popcorn::arch::float_abi::hard_double,
      .return_sentinel = 0xffff'fffe,
      .stack           = 0x2000'8000,
  };
  setup.module->prepare_call(frame);
  CHECK(setup.module->pc() == 0x0800'1000);
  CHECK(setup.module->pending());
  CHECK_FALSE(setup.module->deliverable_now());
  CHECK(setup.module->call_result(frame) == 2);
  auto wide    = frame;
  wide.returns = popcorn::arch::value_kind::i64;
  CHECK(setup.module->call_result(wide) == 0x1'0000'0002);

  auto single = frame;
  single.abi  = popcorn::arch::float_abi::hard_single;
  CHECK_THROWS_AS(setup.module->prepare_call(single), popcorn::config_error);

  setup.module->load(interrupted);
  CHECK(setup.module->pc() == 0x0800'0000);
  CHECK(setup.module->deliverable_now());

  auto open_frame       = frame;
  open_frame.interrupts = true;
  setup.module->prepare_call(open_frame);
  CHECK(setup.module->deliverable_now());
}

TEST_CASE("arch reads an entered call's arguments and returns to the caller")
{
  rig setup;
  setup.module->current.args = {7, 0x1'0000'0009, 0, 0};
  setup.module->current.link = 0x0800'0201;
  setup.module->current.pc   = 0x0800'1000;

  popcorn::arch::call_frame entered{
      .args = {{.kind = popcorn::arch::value_kind::i32}, {.kind = popcorn::arch::value_kind::i64}},
  };
  setup.module->read_args(entered);
  CHECK(entered.args[0].bits == 7);
  CHECK(entered.args[1].bits == 0x1'0000'0009);
  CHECK(setup.module->return_address() == 0x0800'0200);

  setup.module->return_to_caller(
      popcorn::arch::call_arg{.kind = popcorn::arch::value_kind::i32, .bits = 42},
      popcorn::arch::float_abi::soft);
  CHECK(setup.module->pc() == 0x0800'0200);
  CHECK(setup.module->current.args[0] == 42);

  setup.module->current.pc = 0x0800'1000;
  setup.module->return_to_caller(std::nullopt, popcorn::arch::float_abi::soft);
  CHECK(setup.module->pc() == 0x0800'0200);
  CHECK(setup.module->current.args[0] == 42);
}

TEST_CASE("arch-private hooks end the slice or request a reset through services")
{
  rig setup;
  setup.module->wfi_hook();
  CHECK(setup.module->idle());
  CHECK(setup.services.slice_ends == 1);
  CHECK_FALSE(setup.services.stop.has_value());

  setup.module->reset_register_write();
  CHECK(setup.services.reset == popcorn::reset_kind::software);
}

TEST_CASE("arch state round-trips through save and load")
{
  rig setup;
  setup.module->set_irq_line(4, true);
  setup.module->current.sleeping = true;
  std::vector<std::byte> snapshot;
  setup.module->save(snapshot);
  REQUIRE_FALSE(snapshot.empty());

  rig restored;
  CHECK_FALSE(restored.module->pending());
  restored.module->load(snapshot);
  CHECK(restored.module->pending());
  CHECK(restored.module->idle());
  CHECK(restored.module->current.pending == setup.module->current.pending);
}

TEST_CASE("registers and arch enums are described by reflection")
{
  rig setup;
  CHECK(setup.module->registers().size() == 2);
  CHECK(setup.module->registers().front().name == "pc");
  CHECK(std::format("{}", popcorn::arch::memory_op::fetch) == "fetch");
  CHECK(std::format("{}", popcorn::arch::value_kind::f64) == "f64");
  CHECK(std::format("{}", popcorn::arch::float_abi::hard_single) == "hard_single");
}
