#include "machine_impl.hpp"

#include "engine.hpp"
#include "handler_scope.hpp"

#include <popcorn/core/errors.hpp>

#include <reflex/enum.hpp>

#include <algorithm>
#include <format>
#include <limits>
#include <ranges>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace popcorn::core
{
namespace
{
constexpr std::uint64_t tcg_block_limit = 512;

auto add_sat(icount now, std::uint64_t count) noexcept -> icount
{
  constexpr auto most = std::numeric_limits<std::uint64_t>::max();
  return icount{count > most - now.value ? most : now.value + count};
}

auto left(icount bound, icount done) noexcept -> std::uint64_t
{
  return bound.value - std::min(bound.value, done.value);
}

auto memory_op_of(uc_mem_type type) noexcept -> arch::memory_op
{
  switch(type)
  {
    case UC_MEM_READ:
    case UC_MEM_READ_UNMAPPED:
    case UC_MEM_READ_PROT:
    case UC_MEM_READ_AFTER:
      return arch::memory_op::read;
    case UC_MEM_WRITE:
    case UC_MEM_WRITE_UNMAPPED:
    case UC_MEM_WRITE_PROT:
      return arch::memory_op::write;
    case UC_MEM_FETCH:
    case UC_MEM_FETCH_UNMAPPED:
    case UC_MEM_FETCH_PROT:
      return arch::memory_op::fetch;
  }
  return arch::memory_op::read;
}

auto refused_access(uc_err err) noexcept -> bool
{
  return err == UC_ERR_READ_UNMAPPED
      or err == UC_ERR_WRITE_UNMAPPED
      or err == UC_ERR_FETCH_UNMAPPED
      or err == UC_ERR_READ_PROT
      or err == UC_ERR_WRITE_PROT
      or err == UC_ERR_FETCH_PROT;
}

template <typename Run> class running_scope
{
public:
  explicit running_scope(Run& run, hook_registry& hooks) noexcept : run_{run}, hooks_{hooks}
  {
    run_.running   = true;
    hooks_.running = true;
  }
  running_scope(running_scope const&)                    = delete;
  auto operator=(running_scope const&) -> running_scope& = delete;
  ~running_scope()
  {
    run_.clear_slice();
    hooks_.running = false;
    run_.running   = false;
    hooks_.drain();
  }

private:
  Run&           run_;
  hook_registry& hooks_;
};

class engine_scope
{
public:
  explicit engine_scope(bool& in_engine) noexcept : in_engine_{in_engine}
  {
    in_engine_ = true;
  }
  engine_scope(engine_scope const&)                    = delete;
  auto operator=(engine_scope const&) -> engine_scope& = delete;
  ~engine_scope()
  {
    in_engine_ = false;
  }

private:
  bool& in_engine_;
};
}

void machine::run_state::install_hooks()
{
  arch         = owner.arch_.get();
  uc_hook hook = 0;
  check(
      uc_hook_add(
          engine, &hook, UC_HOOK_BLOCK_ICOUNT, reinterpret_cast<void*>(&on_block), this, 1, 0),
      "adding the block hook");
  check(
      uc_hook_add(engine, &hook, UC_HOOK_INTR, reinterpret_cast<void*>(&on_interrupt), this, 1, 0),
      "adding the interrupt hook");
  check(
      uc_hook_add(
          engine, &hook, UC_HOOK_INSN_INVALID, reinterpret_cast<void*>(&on_invalid_insn), this, 1,
          0),
      "adding the invalid instruction hook");
  check(
      uc_hook_add(
          engine, &hook, UC_HOOK_MEM_UNMAPPED | UC_HOOK_MEM_PROT,
          reinterpret_cast<void*>(&on_bad_access), this, 1, 0),
      "adding the refused access hook");
  check(uc_ctl_exits_enable(engine), "enabling exits");

  for(auto const& region : owner.memory_)
  {
    auto const& buffer = *region->buffer;
    if(not buffer.executable or buffer.mappings.size() < 2)
    {
      continue;
    }
    for(auto const& window : buffer.mappings)
    {
      auto const known = std::ranges::any_of(guest_windows, [&](guest_window const& watched) {
        return watched.window.start == window.start;
      });
      if(window.writable and not known)
      {
        guest_windows.push_back({.run = this, .buffer = &buffer, .window = window});
      }
    }
  }
  for(auto& watched : guest_windows)
  {
    check(
        uc_hook_add(
            engine, &hook, UC_HOOK_MEM_WRITE, reinterpret_cast<void*>(&on_guest_store), &watched,
            watched.window.start, watched.window.start + watched.window.size - 1),
        std::format("adding the guest store hook at {:#010x}", watched.window.start));
  }
}

void machine::run_state::reset_machine(reset_kind kind)
{
  owner.scheduler_.restore({}, 0);
  auto fresh            = owner.clock_driver_.state();
  fresh.before_reset_ps = kind == reset_kind::power_on ? 0 : owner.clock_.since_power_on().ps;
  fresh.now             = {};
  fresh.epoch_icount    = {};
  fresh.epoch_time      = {};
  fresh.held            = {};
  fresh.held_time       = {};
  fresh.core_hz         = core_clock_hz(owner.description_, owner.options_);
  fresh.cpi             = owner.options_.cpi;
  owner.clock_driver_.restore(fresh);
  unmapped.reset();
  trap.reset();
  bus_fault.reset();
  frames.clear();
  return_watch = 0;
  resume_past.reset();
  coverage.has_previous = false;
  spin.clear();
  instant_events = 0;
  remove_step_hook();
  owner.arch_->reset(reset_vector);
  std::ranges::fill(lines.active, 0u);
  for(auto const& slot : owner.peripherals_)
  {
    if(slot)
    {
      for(auto& line : slot->irqs)
      {
        line.level_ = false;
      }
      for(auto& line : slot->dma)
      {
        line.level_ = false;
      }
    }
  }
  for(auto const& slot : owner.peripherals_)
  {
    if(slot)
    {
      periph::layer_reset(slot->layout, slot->regs, slot->layer, slot->claimed);
      if(slot->model)
      {
        slot->model->reset(slot->regs, kind);
      }
    }
  }
  owner.trace_.rebase(icount{0});
  rebase_sinks(icount{0});
  ++resets;
}

void machine::run_state::code_changed(
    host_buffer const& buffer,
    std::size_t        first,
    std::size_t        last)
{
  if(in_engine)
  {
    stale_code.push_back({.buffer = &buffer, .first = first, .last = last});
    stop_flag = true;
    return;
  }
  remove_translated(engine, buffer, first, last);
}

auto machine::run_state::payload_pending() const noexcept -> bool
{
  return requested or reset_requested or end_requested or failure or unmapped or trap or bus_fault;
}

void machine::run_state::drain_stale_code()
{
  while(not stale_code.empty())
  {
    auto const& range = stale_code.back();
    remove_translated(engine, *range.buffer, range.first, range.last);
    stale_code.pop_back();
  }
  if(not payload_pending())
  {
    stop_flag = false;
  }
}

auto machine::run_state::wall_expired_now() noexcept -> bool
{
  wall_countdown = wall_check_blocks;
  wall_expired   = std::chrono::steady_clock::now() >= wall_deadline;
  return wall_expired;
}

auto machine::run_state::deliverable() noexcept -> bool
{
  return guarded([this] { return arch->deliverable_now(); });
}

void machine::run_state::on_block(
    uc_struct*    engine,
    std::uint64_t addr,
    std::uint32_t size,
    void*         user) noexcept
{
  auto&      run = *static_cast<run_state*>(user);
  auto const now = run.owner.clock_driver_.exact_now();
  run.block_pc   = static_cast<address>(addr);
  if(run.stop_flag
     or now >= run.deadline
     or now >= run.owner.scheduler_.earliest()
     or (--run.wall_countdown == 0 and run.wall_expired_now())
     or (run.arch->pending() and run.deliverable()))
  {
    run.boundary_stop       = true;
    run.coverage.open_insns = 0;
    uc_emu_stop(engine);
    return;
  }
  if(run.return_watch != 0 and addr == run.return_watch) [[unlikely]]
  {
    run.guarded([&run, addr] {
      run.on_return_block(static_cast<address>(addr));
      return false;
    });
  }
  run.block_size = size;
  if(not run.stepping)
  {
    run.owner.clock_driver_.advance(icount{size});
  }
  if(run.spin_on) [[unlikely]]
  {
    run.guarded([&run, addr] {
      run.spin_block(static_cast<address>(addr));
      return false;
    });
  }
  if(run.coverage_on) [[unlikely]]
  {
    run.guarded([&run, addr, size] {
      run.coverage.count(static_cast<address>(addr), size);
      return false;
    });
    if(run.stepping)
    {
      run.coverage.has_previous = false;
    }
  }
  if(run.sinks_want_block) [[unlikely]]
  {
    run.guarded([&run, addr, size] {
      auto const   pc = static_cast<address>(addr);
      trace_record record{
          .kind = trace_kind::block, .payload = {pc, 0, 0}
      };
      record.aux = static_cast<std::uint16_t>(std::min<std::uint32_t>(size, 0xffff));
      run.emit(record, pc, false);
      return false;
    });
  }
}

void machine::run_state::on_step(
    uc_struct* engine,
    std::uint64_t,
    std::uint32_t,
    void* user) noexcept
{
  auto&      run = *static_cast<run_state*>(user);
  auto const now = run.owner.clock_driver_.exact_now();
  if(run.stop_flag or now >= run.deadline or now >= run.owner.scheduler_.earliest())
  {
    uc_emu_stop(engine);
    return;
  }
  run.owner.clock_driver_.advance(icount{1});
}

void machine::run_state::on_interrupt(uc_struct* engine, std::uint32_t intno, void* user) noexcept
{
  auto& run                 = *static_cast<run_state*>(user);
  run.coverage.has_previous = false;
  run.spin.previous         = {};
  auto const stops          = run.guarded([&] {
    auto const result = run.owner.arch_->on_interrupt(intno);
    run.deadline      = std::min(run.deadline, run.owner.scheduler_.earliest());
    if(result.resumes())
    {
      run.spin.leave();
      return run.stop_on_pending();
    }
    run.trap      = result;
    run.stop_flag = true;
    return true;
  });
  if(stops)
  {
    uc_emu_stop(engine);
  }
}

auto machine::run_state::on_invalid_insn(uc_struct* engine, void* user) noexcept -> bool
{
  auto&      run     = *static_cast<run_state*>(user);
  bool       resumes = false;
  auto const stops   = run.guarded([&] {
    auto const result = run.owner.arch_->on_invalid_insn();
    run.deadline      = std::min(run.deadline, run.owner.scheduler_.earliest());
    resumes           = result.resumes();
    if(not resumes)
    {
      run.trap      = result;
      run.stop_flag = true;
      return false;
    }
    return run.stop_on_pending();
  });
  if(resumes and stops)
  {
    uc_emu_stop(engine);
  }
  return resumes;
}

auto machine::run_state::stop_on_pending() -> bool
{
  if(not(arch->pending() and arch->deliverable_now()))
  {
    return false;
  }
  boundary_stop = true;
  return true;
}

auto machine::run_state::on_bad_access(
    uc_struct*,
    uc_mem_type   type,
    std::uint64_t addr,
    int           size,
    std::int64_t  value,
    void*         user) noexcept -> bool
{
  auto& run = *static_cast<run_state*>(user);
  if(type == UC_MEM_WRITE_PROT)
  {
    auto const* const region = mapped_region::find(
        run.owner.memory_, static_cast<address>(addr), static_cast<std::size_t>(size));
    if(region != nullptr and region->controller != nullptr)
    {
      run.guarded([&] {
        auto const done = run.dispatch_controller_write(
            *region, static_cast<address>(addr), static_cast<std::uint8_t>(size),
            static_cast<std::uint64_t>(value), initiator::cpu);
        if(not done)
        {
          run.record_bus_fault(static_cast<address>(addr), arch::memory_op::write);
        }
        return false;
      });
      uc_ctl_drop_store(run.engine);
      return true;
    }
  }
  run.unmapped  = unmapped_access{.addr = static_cast<address>(addr), .op = memory_op_of(type)};
  run.stop_flag = true;
  return false;
}

void machine::run_state::on_guest_store(
    uc_struct*,
    uc_mem_type,
    std::uint64_t addr,
    int           size,
    std::int64_t,
    void* user) noexcept
{
  auto& watched = *static_cast<guest_window*>(user);
  auto& run     = *watched.run;
  run.guarded([&] {
    auto const first =
        watched.window.offset + static_cast<std::size_t>(addr - watched.window.start);
    run.code_changed(*watched.buffer, first, first + static_cast<std::size_t>(size));
    return true;
  });
}

void machine::run_state::clear_slice() noexcept
{
  for(auto const& range : stale_code)
  {
    try
    {
      remove_translated(engine, *range.buffer, range.first, range.last);
    }
    catch(emulation_error const&)
    {}
  }
  stale_code.clear();
  remove_step_hook();
  requested.reset();
  reset_requested.reset();
  end_requested = false;
  trap.reset();
  unmapped.reset();
  bus_fault.reset();
  failure      = nullptr;
  stop_flag    = false;
  wall_expired = false;
}

void machine::run_state::set_exits(std::vector<address> list)
{
  std::ranges::sort(list);
  if(list == exits)
  {
    return;
  }
  std::vector<std::uint64_t> wide{list.begin(), list.end()};
  check(uc_ctl_set_exits(engine, wide.data(), wide.size()), "setting the exit list");
  std::vector<address> changed;
  std::ranges::set_symmetric_difference(list, exits, std::back_inserter(changed));
  exits = std::move(list);
  for(auto const at : changed)
  {
    check(
        uc_ctl_remove_cache(engine, at, std::uint64_t{at} + 2),
        std::format("removing translated code at exit {:#010x}", at));
  }
}

void machine::run_state::update_stepping(slice_request const& request)
{
  if(stepping
     or not request.exact_instructions
     or left(*request.exact_instructions, executed) > tcg_block_limit)
  {
    return;
  }
  check(
      uc_hook_add(engine, &step_hook, UC_HOOK_CODE, reinterpret_cast<void*>(&on_step), this, 1, 0),
      "adding the stepping hook");
  stepping = true;
}

void machine::run_state::remove_step_hook() noexcept
{
  if(stepping)
  {
    uc_hook_del(engine, step_hook);
    stepping  = false;
    step_hook = 0;
  }
}

auto machine::run_state::next_deadline(slice_request const& request) const -> icount
{
  auto const now      = owner.clock_driver_.exact_now();
  auto const progress = icount{executed.value + jumped.value};
  auto       bound    = add_sat(now, left(request.max_instructions, progress));
  if(request.budget_left)
  {
    bound = std::min(bound, add_sat(now, left(*request.budget_left, executed)));
  }
  if(request.exact_instructions)
  {
    auto const rest = left(*request.exact_instructions, executed);
    bound           = std::min(bound, add_sat(now, stepping ? rest : rest - tcg_block_limit));
  }
  if(request.until_time)
  {
    bound = std::min(bound, owner.clock_.to_icount(*request.until_time));
  }
  return bound;
}

auto machine::run_state::handle_requests() -> std::optional<slice_end>
{
  if(requested)
  {
    auto const asked = *std::exchange(requested, std::nullopt);
    return slice_end{make_stop(asked.reason, asked.detail, std::nullopt)};
  }
  if(reset_requested)
  {
    auto const kind = *std::exchange(reset_requested, std::nullopt);
    if(call or owner.options_.on_reset_request == reset_request_policy::stop)
    {
      return slice_end{make_stop(
          stop_reason::reset_request, std::string{reflex::enum_value_name(kind)}, std::nullopt)};
    }
    reset_machine(kind);
  }
  if(end_requested)
  {
    end_requested = false;
    if(not arch->idle())
    {
      return slice_end{};
    }
  }
  return std::nullopt;
}

auto machine::run_state::dispatch_due(slice_request const& request) -> std::optional<slice_end>
{
  std::uint32_t countdown = wall_check_blocks;
  while(auto const due = owner.scheduler_.pop_due(owner.clock_driver_.exact_now()))
  {
    if(due->id.owner == arch::arch_event_owner)
    {
      arch->on_event(due->id.kind, due->payload);
    }
    else
    {
      auto const& peripherals = owner.peripherals_;
      contract_assert(
          due->id.owner < peripherals.size()
          and peripherals[due->id.owner]
          and peripherals[due->id.owner]->model);
      peripherals[due->id.owner]->model->on_event(due->id.kind, due->payload);
    }
    if(auto end = count_instant(*due))
    {
      return end;
    }
    if(request.stop_when and asks_stop(request))
    {
      return slice_end{make_stop(stop_reason::user, {}, std::nullopt)};
    }
    if(--countdown == 0)
    {
      countdown = wall_check_blocks;
      if(wall_expired_now())
      {
        return slice_end{};
      }
    }
  }
  return std::nullopt;
}

auto machine::run_state::count_instant(event const& due) -> std::optional<slice_end>
{
  auto const now = owner.clock_driver_.exact_now();
  if(now != instant)
  {
    instant        = now;
    instant_events = 0;
  }
  if(++instant_events < max_events_per_instant)
  {
    return std::nullopt;
  }
  instant_events  = 0;
  auto const name = due.id.owner == arch::arch_event_owner
                      ? std::string{"arch"}
                      : owner.peripherals_[due.id.owner]->desc.name;
  return slice_end{make_stop(
      stop_reason::spin,
      std::format("events repeat without time passing: {} kind {}", name, due.id.kind),
      std::nullopt)};
}

auto machine::run_state::asks_stop(slice_request const& request) -> bool
{
  handler_scope const scope{in_handler};
  return request.stop_when();
}

auto machine::run_state::idle_jump(slice_request const& request) -> std::optional<slice_end>
{
  auto const next = owner.scheduler_.next_deadline();
  if(not next and not request.until_time)
  {
    return slice_end{make_stop(stop_reason::idle, {}, std::nullopt)};
  }
  auto const now = owner.clock_driver_.exact_now();
  auto target = add_sat(now, left(request.max_instructions, icount{executed.value + jumped.value}));
  if(next)
  {
    target = std::min(target, *next);
  }
  if(request.until_time)
  {
    target = std::min(target, owner.clock_.to_icount(*request.until_time));
  }
  if(target > now)
  {
    jumped.value += target.value - now.value;
    owner.clock_driver_.jump_to(target);
  }
  return std::nullopt;
}

auto machine::run_state::exit_list(slice_request const& request) const -> std::vector<address>
{
  std::vector<address> list;
  if(request.until)
  {
    list.push_back(*request.until);
  }
  if(call)
  {
    list.push_back(call->frame.return_sentinel);
  }
  return list;
}

auto machine::run_state::leaving_until(slice_request const& request) const -> bool
{
  return request.leave_until and executed.value == 0;
}

void machine::run_state::step_off_until(slice_request const& request)
{
  auto list = exit_list(request);
  list.erase(std::ranges::find(list, *request.until));
  set_exits(std::move(list));
  deadline = std::min(deadline, add_sat(owner.clock_driver_.exact_now(), 1));
}

auto machine::run_state::bound_reached(slice_request const& request) -> std::optional<slice_end>
{
  if(return_watch != 0 and arch->pc() == return_watch)
  {
    on_return_block(return_watch);
  }
  if(call and arch->pc() == call->frame.return_sentinel)
  {
    call->returned = true;
    return slice_end{make_stop(stop_reason::returned, {}, std::nullopt)};
  }
  if(request.budget_left and executed >= *request.budget_left)
  {
    return slice_end{make_stop(stop_reason::budget, {}, std::nullopt)};
  }
  if(request.exact_instructions and executed >= *request.exact_instructions)
  {
    return slice_end{make_stop(stop_reason::budget, {}, std::nullopt)};
  }
  if(request.until_time and owner.clock_driver_.exact_time() >= *request.until_time)
  {
    return slice_end{make_stop(stop_reason::time, {}, std::nullopt)};
  }
  if(request.until and owner.arch_->pc() == *request.until and not leaving_until(request))
  {
    return slice_end{make_stop(stop_reason::address, {}, std::nullopt)};
  }
  if(icount{executed.value + jumped.value} >= request.max_instructions)
  {
    return slice_end{};
  }
  return std::nullopt;
}

auto machine::run_state::trap_after(uc_err err) -> std::optional<slice_end>
{
  if(bus_fault)
  {
    auto const access = *std::exchange(bus_fault, std::nullopt);
    unmapped.reset();
    trap.reset();
    auto const result = arch->on_unmapped(access.addr, access.op);
    if(result.resumes())
    {
      return std::nullopt;
    }
    return stop_from(result);
  }
  if(trap)
  {
    auto const result = *std::exchange(trap, std::nullopt);
    unmapped.reset();
    return stop_from(result);
  }
  if(err == UC_ERR_OK)
  {
    return std::nullopt;
  }
  if(refused_access(err) and unmapped)
  {
    auto const access = *std::exchange(unmapped, std::nullopt);
    auto const result = owner.arch_->on_unmapped(access.addr, access.op);
    if(result.resumes())
    {
      return std::nullopt;
    }
    return stop_from(result);
  }
  throw emulation_error(
      std::format("engine stopped at {:#010x}: {}", owner.arch_->pc(), uc_strerror(err)));
}

auto machine::run_state::engine_run() -> uc_err
{
  engine_scope const scope{in_engine};
  return uc_emu_start(engine, owner.arch_->resume_address(), 0, 0, 0);
}

void machine::run_state::rethrow_failure()
{
  if(failure)
  {
    std::rethrow_exception(std::exchange(failure, nullptr));
  }
}

auto machine::run_state::make_stop(
    stop_reason               reason,
    std::string               detail,
    std::optional<fault_info> fault) const -> stop
{
  return stop{
      .reason       = reason,
      .where        = {.pc = owner.arch_->pc()},
      .instructions = owner.clock_.now(),
      .time         = owner.clock_.time(),
      .fault        = fault,
      .detail       = std::move(detail),
  };
}

auto machine::run_state::stop_from(arch::trap_result const& result) const -> slice_end
{
  return slice_end{make_stop(*result.reason(), {}, result.fault_detail())};
}

auto machine::run_state::finish(slice_request const& request, slice_end end) -> slice_result
{
  flush_sinks();
  if(not end.stopped and request.stop_when and asks_stop(request))
  {
    end.stopped = make_stop(stop_reason::user, {}, std::nullopt);
  }
  if(end.stopped)
  {
    auto& where = end.stopped->where;
    where       = locate(where.pc);
    emit(
        {
            .kind    = trace_kind::stop,
            .aux     = static_cast<std::uint16_t>(std::to_underlying(end.stopped->reason)),
            .payload = {where.pc, 0, 0}
    },
        where.pc, true);
  }
  flush_sinks();
  return slice_result{.stopped = std::move(end.stopped), .executed = executed};
}

auto machine::run_state::run(slice_request const& request) -> slice_result
{
  if(running)
  {
    throw machine_busy("a slice is already running");
  }
  if(invalid)
  {
    throw emulation_error("the machine was left invalid by a failed restore");
  }
  running_scope const scope{*this, *owner.hooks_};
  executed       = {};
  jumped         = {};
  wall_countdown = wall_check_blocks;
  wall_expired   = false;
  wall_deadline  = std::chrono::steady_clock::now() + request.wall_budget;
  set_exits(exit_list(request));
  for(;;)
  {
    drain_stale_code();
    if(auto end = handle_requests())
    {
      return finish(request, std::move(*end));
    }
    if(auto end = dispatch_due(request))
    {
      return finish(request, std::move(*end));
    }
    if(auto end = handle_requests())
    {
      return finish(request, std::move(*end));
    }
    if(auto end = bound_reached(request))
    {
      return finish(request, std::move(*end));
    }
    auto const before      = coverage_on ? arch->pc() : address{};
    auto const spin_before = spin_on ? arch->pc() : address{};
    auto const spin_asleep = spin_on and arch->idle();
    if(auto const entry = arch->deliver_pending(); not entry.resumes())
    {
      return finish(request, stop_from(entry));
    }
    if(coverage_on and arch->pc() != before)
    {
      coverage.has_previous = false;
    }
    if(spin_on and arch->pc() != spin_before)
    {
      auto const after = arch->pc();
      auto const woke  = spin_asleep and after > spin_before and after - spin_before <= 4;
      spin.previous    = {};
      if(not woke)
      {
        spin.enter();
      }
    }
    if(arch->idle())
    {
      spin.progress();
      if(auto end = idle_jump(request))
      {
        return finish(request, std::move(*end));
      }
      continue;
    }
    if(resume_past and arch->pc() != resume_past->first)
    {
      resume_past.reset();
    }
    update_stepping(request);
    deadline = std::min(next_deadline(request), owner.scheduler_.earliest());
    if(not boundary_stop)
    {
      coverage.has_previous = false;
      spin.previous         = {};
    }
    boundary_stop      = false;
    auto const leaving = leaving_until(request) and request.until and arch->pc() == *request.until;
    if(leaving)
    {
      step_off_until(request);
    }
    auto const started = owner.clock_driver_.exact_now();
    auto const err     = engine_run();
    if(std::exchange(precise_exit, false))
    {
      take_precise_exit();
    }
    auto const ran = owner.clock_driver_.exact_now().value - started.value;
    if(coverage_on)
    {
      auto const pc = arch->pc();
      coverage.leave(*arch, pc, std::ranges::contains(exit_list(request), pc));
    }
    executed.value += ran;
    if(leaving)
    {
      set_exits(exit_list(request));
    }
    if(ran == 0)
    {
      std::ignore = wall_expired_now();
    }
    rethrow_failure();
    auto const resets_before = resets;
    if(auto end = handle_requests())
    {
      trap.reset();
      unmapped.reset();
      return finish(request, std::move(*end));
    }
    if(resets != resets_before)
    {
      continue;
    }
    if(auto end = trap_after(err))
    {
      return finish(request, std::move(*end));
    }
    if(wall_expired)
    {
      return finish(request, {});
    }
  }
}

auto machine::run_slice(slice_request const& request) -> slice_result
{
  return run_->run(request);
}

void machine::do_request_stop(stop_reason reason, std::string detail)
{
  if(not run_->requested)
  {
    run_->requested = requested_stop{.reason = reason, .detail = std::move(detail)};
  }
  run_->stop_flag = true;
}

void machine::do_request_reset(reset_kind kind)
{
  if(not run_->reset_requested)
  {
    run_->reset_requested = kind;
  }
  run_->stop_flag = true;
}

void machine::do_end_slice()
{
  run_->end_requested = true;
  run_->stop_flag     = true;
}
}
