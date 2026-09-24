#include <popcorn/arch/arch.hpp>
#include <popcorn/core/errors.hpp>
#include <popcorn/core/intercept.hpp>
#include <popcorn/core/log.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/core/memory_view.hpp>
#include <popcorn/core/register_view.hpp>
#include <popcorn/core/snapshot.hpp>
#include <popcorn/image/image.hpp>
#include <popcorn/periph/model.hpp>

#include "common.hpp"

#include <doctest/doctest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

namespace popcorn::consumers::facade
{
auto run_until(core::machine& target, address until, icount budget) -> std::optional<stop>
{
  core::slice_request request;
  request.until       = until;
  request.budget_left = budget;
  request.wall_budget = std::chrono::milliseconds{1};
  request.stop_when   = [&target] { return target.virtual_clock().time() >= vtime{1'000'000}; };
  while(true)
  {
    auto const slice = target.run_slice(request);
    if(slice.stopped)
    {
      return slice.stopped;
    }
    request.budget_left = icount{request.budget_left->value - slice.executed.value};
  }
}

auto step(core::machine& target, std::uint64_t count) -> std::optional<stop>
{
  core::slice_request request;
  request.exact_instructions = icount{count};
  return target.run_slice(request).stopped;
}

auto call_function(core::machine& target, image::image const& loaded) -> core::call_result
{
  arch::call_frame const frame{
      .entry = loaded.lookup("drc_process").value,
      .args =
          {{.kind = arch::value_kind::i32, .bits = 0x2000'0000},
                 {.kind = arch::value_kind::f32, .bits = 0x3fc0'0000}},
      .returns         = arch::value_kind::f32,
      .abi             = loaded.abi,
      .return_sentinel = 0xffff'fffe,
      .stack           = std::nullopt,
      .privileged      = true,
      .interrupts      = false,
  };
  target.begin_call(frame);
  while(not target.run_slice({}).stopped)
  {
  }
  std::ignore = target.in_call();
  std::ignore = target.end_call();
  return target.call(frame, icount{2'000'000});
}

auto hooks(core::machine& target) -> std::vector<core::hook_handle>
{
  std::vector<core::hook_handle> handles;
  handles.push_back(target.add_intercept(0x0800'0200, [](core::intercept_call const& call) {
    std::array const kinds{arch::value_kind::i32, arch::value_kind::f64};
    auto const       args = call.args(kinds);
    std::ignore           = call.target().mem().load<std::uint32_t>(static_cast<address>(args[0]));
    if(call.occurrence() > 3)
    {
      return core::intercept_result::stop(stop_reason::user);
    }
    return call.function() == call.return_address()
             ? core::intercept_result::proceed()
             : core::intercept_result::returns(
                   arch::call_arg{.kind = arch::value_kind::i32, .bits = 0});
  }));
  handles.push_back(target.add_return_hook(
      0x0800'0300, {arch::value_kind::i32}, arch::value_kind::i32,
      [](core::returned_call const& call) {
        std::ignore = call.value <= call.args.at(0) and call.instructions < icount{1000};
      }));
  handles.push_back(target.add_intercept(
      0x0800'0400,
      [](core::intercept_call const&) { return core::intercept_result::returns(std::nullopt); },
      core::intercept_mode::once));
  handles.push_back(target.add_log_sink([](core::log_record const& record) {
    std::ignore = record.kind == core::log_kind::unmodelled_access and record.write;
  }));
  handles.front().reset();
  handles.push_back(
      target.add_watch(0x2000'0000, 4, core::watch_kind::write, [](core::watch_hit const& hit) {
        return hit.write and hit.by == initiator::dma ? core::watch_action::stop
                                                      : core::watch_action::resume;
      }));
  return handles;
}

void views(core::machine& target)
{
  auto const memory = target.mem();
  std::ignore       = memory.store<std::uint32_t>(0x2000'0000, 5);
  std::ignore       = memory.load<float>(0x2000'0004);
  std::array<std::byte, 16> buffer{};
  std::ignore = memory.read_block(0x2000'0000, buffer);
  std::ignore = memory.write_block(0x2000'0000, buffer);
  std::ignore = memory.read(0x4001'3800, 4);
  std::ignore = memory.write(0x4001'3800, 1, 0x41);

  auto const registers = target.regs();
  for(auto const& reg : registers.list())
  {
    std::ignore = registers.read(reg);
  }
  if(auto const r0 = registers.find("r0"))
  {
    registers.write(*r0, 1);
  }
  std::ignore = registers.pc();
  std::ignore = target.peripheral("UART1").peek(0);
  if(auto* const uart = target.model("UART1"))
  {
    std::ignore = uart->claims();
  }
}

auto snapshots(core::machine& target) -> core::snapshot
{
  auto const taken = target.snapshot();
  target.restore(taken);
  auto const saved = core::encode_snapshot(taken.data());
  target.restore(core::snapshot{core::decode_snapshot(saved)});
  return taken;
}

void boot(core::machine& target)
{
  target.load(image::load_elf("build/bootloader.elf"));
  target.load(image::load_elf("build/b2r.elf"));
  for(auto const& loaded : target.loaded_images())
  {
    auto const where = loaded.locate(0x0800'0100);
    std::ignore      = where.symbol.empty() or where.line == 0;
  }
  target.reset();
  std::ignore = target.generation();
  std::ignore = target.running();
  std::ignore = target.description().part;
  periph::register_model("usart", "v2.*", [](periph::model_context& ctx, peripheral_desc const&) {
    ctx.request_stop(stop_reason::user, "python model");
    return std::shared_ptr<periph::model>{};
  });
}
}

TEST_CASE("Python facade consumer drives every machine entry point")
{
  popcorn::core::machine_options options;
  options.on_reset_request   = popcorn::core::reset_request_policy::reset_and_continue;
  options.faults             = popcorn::arch::fault_policy::enter_handler;
  options.spin_threshold     = 1000;
  options.strict_peripherals = true;
  popcorn::core::machine configured{
      popcorn::testing::toy_description(), popcorn::testing::unused_arch_factory(), options};
  CHECK_THROWS_AS(popcorn::consumers::facade::step(configured, 1), popcorn::not_implemented);
  popcorn::core::machine target{
      popcorn::testing::toy_description(), popcorn::testing::unused_arch_factory(), {}};
  CHECK_THROWS_AS(
      popcorn::consumers::facade::run_until(target, 0x0800'0100, popcorn::icount{1000}),
      popcorn::not_implemented);
  CHECK_THROWS_AS(popcorn::consumers::facade::hooks(target), popcorn::not_implemented);
  CHECK_THROWS_AS(popcorn::consumers::facade::views(target), popcorn::not_implemented);
  CHECK_THROWS_AS(popcorn::consumers::facade::snapshots(target), popcorn::not_implemented);
  CHECK_THROWS_AS(popcorn::consumers::facade::boot(target), popcorn::not_implemented);
  CHECK_THROWS_AS(
      popcorn::consumers::facade::call_function(target, popcorn::image::image{}),
      popcorn::not_implemented);
  CHECK_FALSE(target.running());
}
