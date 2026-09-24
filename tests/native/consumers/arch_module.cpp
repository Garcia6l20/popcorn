#include <popcorn/arch/arch.hpp>
#include <popcorn/core/clock.hpp>
#include <popcorn/core/errors.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/core/scheduler.hpp>
#include <popcorn/core/services.hpp>
#include <popcorn/core/trace.hpp>

#include "../fake_services.hpp"
#include "common.hpp"

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <tuple>
#include <vector>

namespace popcorn::consumers::arch_module
{
class armv8m_shape final : public popcorn::arch::arch
{
public:
  void attach(popcorn::arch::arch_context const& ctx) override
  {
    if(not ctx.description.core.fpu and ctx.description.core.nvic_prio_bits > 8)
    {
      throw config_error("desc", 0, "core.nvic_prio_bits", "at most 8");
    }
    context.emplace(ctx);
  }

  void reset(address reset_vector) override
  {
    std::array<std::byte, 8> vectors{};
    std::ignore = context->services.read_block(reset_vector, vectors, initiator::cpu);
    systick     = context->events.schedule(
        context->clock.to_icount(vtime{context->clock.time().ps + 1'000'000}), 0, 0);
    set_pending(false);
  }

  auto deliver_pending() -> popcorn::arch::trap_result override
  {
    auto const stacked =
        context->services.write_block(0x2000'0000, std::span<std::byte const>{}, initiator::cpu);
    if(not stacked)
    {
      return popcorn::arch::trap_result::fault({fault_kind::bus, stacked.error().at, 0, 0});
    }
    context->services.trace(
        {.kind = core::trace_kind::irq, .flags = 1, .aux = 15, .dt = 0, .payload = {}});
    return popcorn::arch::trap_result::resume();
  }

  [[nodiscard]] auto deliverable_now() const -> bool override
  {
    return true;
  }

  void set_irq_line(std::uint32_t, bool active) override
  {
    set_pending(active);
  }

  auto on_interrupt(std::uint32_t intno) -> popcorn::arch::trap_result override
  {
    return intno == 7 ? popcorn::arch::trap_result::stop(stop_reason::user)
                      : popcorn::arch::trap_result::resume();
  }

  auto on_invalid_insn() -> popcorn::arch::trap_result override
  {
    if(context->faults == popcorn::arch::fault_policy::enter_handler)
    {
      return popcorn::arch::trap_result::resume();
    }
    return popcorn::arch::trap_result::fault({fault_kind::usage, 0, 0x1'0000, 0});
  }

  auto on_unmapped(address addr, popcorn::arch::memory_op) -> popcorn::arch::trap_result override
  {
    auto const read = context->services.read(0xe000'ed28, 4, initiator::cpu);
    std::ignore     = context->services.write(0xe000'ed38, 4, addr, initiator::cpu);
    return popcorn::arch::trap_result::fault({fault_kind::bus, addr, read.value_or(0), 0});
  }

  void on_event(std::uint32_t, std::uint64_t) override
  {
    context->events.cancel(systick);
    systick = context->events.schedule(context->clock.now(), 0, 0);
    if(sleeping)
    {
      context->services.end_slice();
    }
  }

  [[nodiscard]] auto idle() const -> bool override
  {
    return sleeping;
  }

  [[nodiscard]] auto pc() const -> address override
  {
    return 0;
  }

  [[nodiscard]] auto resume_address() const -> address override
  {
    return pc() | 1;
  }

  [[nodiscard]] auto registers() const -> std::span<popcorn::arch::cpu_register const> override
  {
    return core_registers;
  }

  void prepare_call(popcorn::arch::call_frame const& frame) override
  {
    if(frame.abi == popcorn::arch::float_abi::hard_single)
    {
      throw config_error("image", 0, "abi", "soft or hard_double");
    }
  }

  [[nodiscard]] auto call_result(popcorn::arch::call_frame const&) const -> std::uint64_t override
  {
    return 0;
  }

  void read_args(popcorn::arch::call_frame&) const override
  {}

  [[nodiscard]] auto return_address() const -> address override
  {
    return 0;
  }

  void return_to_caller(std::optional<popcorn::arch::call_arg> const&, popcorn::arch::float_abi)
      override
  {}

  void save(std::vector<std::byte>& out) const override
  {
    out.push_back(std::byte{sleeping});
  }

  void load(std::span<std::byte const> in) override
  {
    sleeping = not in.empty() and in.front() != std::byte{0};
  }

  void sysresetreq()
  {
    context->services.request_reset(reset_kind::software);
  }

  void lockup()
  {
    context->services.request_stop(stop_reason::fault, "lockup");
  }

  static constexpr std::array core_registers{
      popcorn::arch::cpu_register{"pc", 11, 32}
  };

  std::optional<popcorn::arch::arch_context> context;
  std::uint64_t                              systick{};
  bool                                       sleeping{};
};

auto factory() -> popcorn::arch::arch_factory
{
  return [](core_desc const&) -> std::unique_ptr<popcorn::arch::arch> {
    return std::make_unique<armv8m_shape>();
  };
}
}

TEST_CASE("arch module consumer implements arch through its context")
{
  auto const                      description = popcorn::testing::toy_description();
  popcorn::core::clock            clock{description.core.reset_clock_hz};
  popcorn::core::scheduler        queue;
  popcorn::core::event_port       events{queue, popcorn::arch::arch_event_owner};
  popcorn::testing::fake_services services;
  auto const module = popcorn::consumers::arch_module::factory()(description.core);
  module->attach(
      {nullptr, description, clock, events, services, popcorn::arch::fault_policy::enter_handler});
  CHECK(module->on_invalid_insn().resumes());
  module->reset(0x0800'0000);
  CHECK(queue.pending().size() == 1);
  CHECK(module->deliver_pending().resumes());
  CHECK(module->on_unmapped(0x6000'0000, popcorn::arch::memory_op::read).fault_detail());

  popcorn::core::machine target{description, popcorn::consumers::arch_module::factory(), {}};
  CHECK_THROWS_AS(target.reset(popcorn::reset_kind::software), popcorn::not_implemented);
  CHECK_THROWS_AS(
      target.run_slice({.max_instructions = popcorn::icount{1000}}), popcorn::not_implemented);
  CHECK_THROWS_AS(
      target.call({.entry = 0x0800'0100, .return_sentinel = 0xffff'fffe}, popcorn::icount{100}),
      popcorn::not_implemented);
}
