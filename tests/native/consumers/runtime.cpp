#include <popcorn/arch/arch.hpp>
#include <popcorn/core/clock.hpp>
#include <popcorn/core/errors.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/core/scheduler.hpp>
#include <popcorn/core/snapshot.hpp>
#include <popcorn/core/trace.hpp>
#include <popcorn/core/version.hpp>
#include <popcorn/periph/model.hpp>
#include <popcorn/periph/register_file.hpp>

#include "common.hpp"

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace core = popcorn::core;

namespace popcorn::consumers::runtime
{
void run_loop_calls(popcorn::arch::arch& module, core::clock_driver& driver, core::scheduler& queue)
{
  auto const entry = module.deliver_pending();
  if(not entry.resumes())
  {
    return;
  }
  std::ignore = module.resume_address();
  driver.advance(popcorn::icount{5});
  if(driver.state().now >= queue.earliest() and module.pending() and module.deliverable_now())
  {
    return;
  }
  while(auto const due = queue.pop_due(driver.state().now))
  {
    if(due->id.owner == popcorn::arch::arch_event_owner)
    {
      module.on_event(due->id.kind, due->payload);
    }
  }
  std::ignore = module.on_interrupt(8);
  std::ignore = module.on_invalid_insn();
  std::ignore = module.on_unmapped(0x6000'0000, popcorn::arch::memory_op::fetch);
  if(module.idle())
  {
    if(auto const next = queue.next_deadline())
    {
      driver.jump_to(*next);
    }
  }
  module.set_irq_line(3, true);
}

auto snapshot_calls(
    popcorn::arch::arch&            module,
    core::clock_driver&             driver,
    core::scheduler&                queue,
    popcorn::periph::model&         model,
    popcorn::periph::register_file& regs) -> core::snapshot_data
{
  core::snapshot_data data;
  data.format = core::snapshot_format_version;
  for(auto const& reg : module.registers())
  {
    data.registers.push_back({std::string{reg.name}, 0});
  }
  data.arch.owner = "arch";
  module.save(data.arch.data);
  data.models.push_back({regs.description().name, {}});
  model.save(data.models.back().data);
  data.popcorn_version = std::string{core::version()};
  data.peripherals.push_back({regs.description().name, {regs.peek(0)}, {1}, {0}});
  auto const pending = queue.pending();
  data.events        = {
      {pending.begin(), pending.end()},
      queue.next_seq()
  };
  data.clock = driver.state();

  module.load(data.arch.data);
  regs.poke(0, data.peripherals.front().words.front());
  model.load(data.models.front().data);
  queue.restore(data.events.pending, data.events.next_seq);
  driver.restore(data.clock);
  return core::decode_snapshot(core::encode_snapshot(data));
}

void call_calls(popcorn::arch::arch& module)
{
  popcorn::arch::call_frame frame{.entry = 0x0800'0100, .return_sentinel = 0xffff'fffe};
  module.prepare_call(frame);
  std::ignore = module.call_result(frame);
  module.read_args(frame);
  std::ignore = module.return_address();
  module.return_to_caller(std::nullopt, frame.abi);
}

void model_calls(popcorn::periph::model& model, popcorn::periph::register_file& regs)
{
  std::ignore = model.claims();
  model.reset(regs, popcorn::reset_kind::power_on);
  popcorn::periph::access_ctx const ctx{.pc = 0x0800'0100, .now = popcorn::icount{1}};
  std::ignore = model.on_read(regs, 0, 4, ctx);
  std::ignore = model.on_write(regs, 0, 4, 0, 1, ctx);
  model.on_event(0, 0);
}

void services_calls(core::machine& target)
{
  target.trace().append({.kind = core::trace_kind::irq, .flags = 1, .aux = 15}, popcorn::icount{3});
  auto const id = popcorn::flat_register_id(target.description(), {.peripheral = 0, .reg = 1});
  target.trace().append(
      {
          .kind    = core::trace_kind::mmio,
          .flags   = 4 | core::trace_write_flag,
          .aux     = core::trace_id(id),
          .payload = {0x0800'0100, 0x4001'3804, 1}
  },
      popcorn::icount{5});
  target.trace().rebase(popcorn::icount{2});
  target.trace().append(
      {
          .kind    = core::trace_kind::stop,
          .aux     = static_cast<std::uint16_t>(popcorn::stop_reason::reset_request),
          .payload = {0x0800'0200, 0, 0}
  },
      popcorn::icount{4});
}

auto policies(core::machine_options const& options) -> bool
{
  return options.on_reset_request
      == core::reset_request_policy::reset_and_continue
      or options.faults
      == popcorn::arch::fault_policy::enter_handler
      or options.spin_threshold
       > 0
      or options.strict_peripherals;
}
}

TEST_CASE("runtime consumer links against the interfaces it implements")
{
  core::machine target{
      popcorn::testing::toy_description(), popcorn::testing::unused_arch_factory(), {}};
  popcorn::consumers::runtime::services_calls(target);
  CHECK(target.trace().size() == 4);
  CHECK_FALSE(popcorn::consumers::runtime::policies({}));

  core::clock        clock{4'000'000};
  core::clock_driver driver{clock};
  core::scheduler    queue;
  core::event_port   arch_events{queue, popcorn::arch::arch_event_owner};
  arch_events.schedule(popcorn::icount{10}, 1, 0);
  driver.advance(popcorn::icount{10});
  auto const due = queue.pop_due(clock.now());
  REQUIRE(due.has_value());
  CHECK(due->id.owner == popcorn::arch::arch_event_owner);
}
