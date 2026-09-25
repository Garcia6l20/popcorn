#include "machine_impl.hpp"

#include "engine.hpp"

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
  return err
      == UC_ERR_READ_UNMAPPED
      or err
      == UC_ERR_WRITE_UNMAPPED
      or err
      == UC_ERR_FETCH_UNMAPPED
      or err
      == UC_ERR_READ_PROT
      or err
      == UC_ERR_WRITE_PROT
      or err
      == UC_ERR_FETCH_PROT;
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

void machine::run_state::reset_machine(reset_kind)
{
  owner.scheduler_.restore({}, 0);
  auto fresh         = owner.clock_driver_.state();
  fresh.now          = {};
  fresh.epoch_icount = {};
  fresh.epoch_time   = {};
  fresh.core_hz      = core_clock_hz(owner.description_, owner.options_);
  fresh.cpi          = owner.options_.cpi;
  owner.clock_driver_.restore(fresh);
  unmapped.reset();
  trap.reset();
  remove_step_hook();
  owner.arch_->reset(reset_vector);
  owner.trace_.rebase(icount{0});
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
  return requested or reset_requested or end_requested or failure or unmapped or trap;
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
    uc_struct* engine,
    std::uint64_t,
    std::uint32_t size,
    void*         user) noexcept
{
  auto&      run = *static_cast<run_state*>(user);
  auto const now = run.owner.clock_.now();
  if(run.stop_flag
     or now
     >= run.deadline
     or now
     >= run.owner.scheduler_.earliest()
     or (--run.wall_countdown == 0 and run.wall_expired_now())
     or (run.arch->pending() and run.deliverable()))
  {
    uc_emu_stop(engine);
    return;
  }
  if(not run.stepping)
  {
    run.owner.clock_driver_.advance(icount{size});
  }
}

void machine::run_state::on_step(
    uc_struct* engine,
    std::uint64_t,
    std::uint32_t,
    void* user) noexcept
{
  auto&      run = *static_cast<run_state*>(user);
  auto const now = run.owner.clock_.now();
  if(run.stop_flag or now >= run.deadline or now >= run.owner.scheduler_.earliest())
  {
    uc_emu_stop(engine);
    return;
  }
  run.owner.clock_driver_.advance(icount{1});
}

void machine::run_state::on_interrupt(uc_struct* engine, std::uint32_t intno, void* user) noexcept
{
  auto&      run   = *static_cast<run_state*>(user);
  auto const stops = run.guarded([&] {
    auto const result = run.owner.arch_->on_interrupt(intno);
    run.deadline      = std::min(run.deadline, run.owner.scheduler_.earliest());
    if(result.resumes())
    {
      return false;
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

auto machine::run_state::on_invalid_insn(uc_struct*, void* user) noexcept -> bool
{
  auto& run     = *static_cast<run_state*>(user);
  bool  resumes = false;
  run.guarded([&] {
    auto const result = run.owner.arch_->on_invalid_insn();
    run.deadline      = std::min(run.deadline, run.owner.scheduler_.earliest());
    resumes           = result.resumes();
    if(not resumes)
    {
      run.trap      = result;
      run.stop_flag = true;
    }
    return not resumes;
  });
  return resumes;
}

auto machine::run_state::on_bad_access(
    uc_struct*,
    uc_mem_type   type,
    std::uint64_t addr,
    int,
    std::int64_t,
    void* user) noexcept -> bool
{
  auto& run     = *static_cast<run_state*>(user);
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
     or left(*request.exact_instructions, executed)
     > tcg_block_limit)
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
  auto const now      = owner.clock_.now();
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
  while(auto const due = owner.scheduler_.pop_due(owner.clock_.now()))
  {
    contract_assert(due->id.owner == arch::arch_event_owner);
    arch->on_event(due->id.kind, due->payload);
    if(request.stop_when and request.stop_when())
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

auto machine::run_state::idle_jump(slice_request const& request) -> std::optional<slice_end>
{
  auto const next = owner.scheduler_.next_deadline();
  if(not next)
  {
    return slice_end{make_stop(stop_reason::idle, {}, std::nullopt)};
  }
  auto const now    = owner.clock_.now();
  auto       target = std::min(
      *next, add_sat(now, left(request.max_instructions, icount{executed.value + jumped.value})));
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

auto machine::run_state::bound_reached(slice_request const& request) -> std::optional<slice_end>
{
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
  if(request.until_time and owner.clock_.time() >= *request.until_time)
  {
    return slice_end{make_stop(stop_reason::time, {}, std::nullopt)};
  }
  if(request.until and owner.arch_->pc() == *request.until)
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

auto machine::run_state::finish(slice_request const& request, slice_end end) const -> slice_result
{
  if(not end.stopped and request.stop_when and request.stop_when())
  {
    end.stopped = make_stop(stop_reason::user, {}, std::nullopt);
  }
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
    if(auto const entry = arch->deliver_pending(); not entry.resumes())
    {
      return finish(request, stop_from(entry));
    }
    if(arch->idle())
    {
      if(auto end = idle_jump(request))
      {
        return finish(request, std::move(*end));
      }
      continue;
    }
    update_stepping(request);
    deadline           = std::min(next_deadline(request), owner.scheduler_.earliest());
    auto const started = owner.clock_.now();
    auto const err     = engine_run();
    auto const ran     = owner.clock_.now().value - started.value;
    executed.value += ran;
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
