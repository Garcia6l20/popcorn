#include "hooks.hpp"

#include "machine.hpp"
#include "python_callable.hpp"

#include <popcorn/core/errors.hpp>

#include <nanobind/nanobind.h>
#include <nanobind/stl/optional.h>
#include <nanobind/stl/vector.h>

#include <format>
#include <iterator>
#include <stdexcept>
#include <utility>

namespace popcorn::bind
{
namespace
{
namespace nb = reflex::py::nb;

struct view_scope
{
  view_scope()                                     = default;
  view_scope(view_scope const&)                    = delete;
  auto operator=(view_scope const&) -> view_scope& = delete;
  ~view_scope()
  {
    detail::clear_alive(*alive);
  }

  std::shared_ptr<std::atomic<bool>> alive = detail::make_alive();
};
}

hook::hook(core::hook_handle handle, detail::host_ref host) noexcept
    : handle_{std::move(handle)}, host_{std::move(host)}
{}

hook::~hook()
{
  if(handle_.active())
  {
    detail::retire(host_, [owned = std::move(handle_)] mutable { owned.reset(); });
  }
}

void hook::remove()
{
  if(not handle_.active())
  {
    handle_.reset();
    return;
  }
  detail::host_access(host_, [this] { handle_.reset(); });
}

void hook::retire() noexcept
{
  if(handle_.active())
  {
    detail::retire(host_, [owned = std::move(handle_)] mutable { owned = core::hook_handle{}; });
  }
}

auto hook::active() const noexcept -> bool
{
  return handle_.active();
}

call_view::call_view(core::intercept_call const& call, detail::alive_flag alive) noexcept
    : call_{&call}, alive_{std::move(alive)}
{}

auto call_view::call() const -> core::intercept_call const&
{
  if(not detail::is_alive(alive_))
  {
    throw config_error("intercept", 0, "call", "a call read inside its handler");
  }
  return *call_;
}

auto call_view::function() const -> address
{
  return call().function();
}

auto call_view::return_address() const -> address
{
  return call().return_address();
}

auto call_view::occurrence() const -> std::uint64_t
{
  return call().occurrence();
}

auto call_view::args(std::vector<arch::value_kind> const& kinds, std::optional<arch::float_abi> abi)
    const -> std::vector<std::uint64_t>
{
  return call().args(kinds, abi);
}

auto machine_handle::add_intercept(address at, nb::object handler, bool once) -> hook
{
  auto callable = detail::held_bare(std::move(handler));
  auto handle   = access([&](core::machine& target) {
    return target.add_intercept(
        at,
        [callable](core::intercept_call const& call) {
          detail::gil_entry const locked;
          view_scope const        scope;
          return nb::cast<core::intercept_result>(
              detail::call_python([&] { return (*callable)(call_view{call, scope.alive}); }));
        },
        once ? core::intercept_mode::once : core::intercept_mode::every);
  });
  return hook{std::move(handle), slot_};
}

auto machine_handle::add_return_hook(
    address                              at,
    std::vector<arch::value_kind> const& args,
    arch::value_kind                     returns,
    nb::object                           handler) -> hook
{
  auto callable = detail::held_bare(std::move(handler));
  auto handle   = access([&](core::machine& target) {
    return target.add_return_hook(at, args, returns, [callable](core::returned_call const& done) {
      detail::gil_entry const locked;
      detail::call_python([&] {
        return (*callable)(returned_record{
            .function     = done.function,
            .occurrence   = done.occurrence,
            .args         = done.args,
            .value        = done.value,
            .instructions = done.instructions.value,
        });
      });
    });
  });
  return hook{std::move(handle), slot_};
}

auto machine_handle::add_return_log(
    address                              at,
    std::vector<arch::value_kind> const& args,
    arch::value_kind                     returns,
    return_log const&                    log) -> hook
{
  auto handle = access([&](core::machine& target) {
    return target.add_return_hook(
        at, args, returns, [log, clock = &target.virtual_clock()](core::returned_call const& done) {
          log.append({
              .time_ps      = clock->time().ps,
              .occurrence   = done.occurrence,
              .args         = done.args,
              .value        = done.value,
              .instructions = done.instructions.value,
          });
        });
  });
  return hook{std::move(handle), slot_};
}

return_log::return_log(std::uint64_t capacity) : state_{std::make_shared<state>()}
{
  if(capacity == 0)
  {
    throw std::invalid_argument("a return log holds at least 1 entry");
  }
  state_->capacity = capacity;
}

auto return_log::mark() const -> std::uint64_t
{
  std::scoped_lock const lock{state_->mutex};
  return state_->total;
}

auto return_log::since(std::uint64_t mark) const -> std::vector<logged_return>
{
  std::scoped_lock const lock{state_->mutex};
  auto const             first = state_->total - state_->entries.size();
  auto const             skip  = std::min<std::uint64_t>(
      state_->entries.size(), mark > first ? mark - first : std::uint64_t{0});
  return {
      std::next(state_->entries.begin(), static_cast<std::ptrdiff_t>(skip)), state_->entries.end()};
}

auto return_log::dropped() const -> std::uint64_t
{
  std::scoped_lock const lock{state_->mutex};
  return state_->dropped;
}

void return_log::clear()
{
  std::scoped_lock const lock{state_->mutex};
  state_->entries.clear();
}

void return_log::stop()
{
  std::scoped_lock const lock{state_->mutex};
  state_->stopped = true;
}

void return_log::append(logged_return entry) const
{
  std::scoped_lock const lock{state_->mutex};
  if(state_->stopped)
  {
    return;
  }
  ++state_->total;
  if(state_->entries.size() == state_->capacity)
  {
    state_->entries.pop_front();
    ++state_->dropped;
  }
  state_->entries.push_back(std::move(entry));
}

auto machine_handle::add_watch(
    address          at,
    std::uint32_t    size,
    core::watch_kind kind,
    nb::object       handler) -> hook
{
  if(size == 0 or std::uint64_t{at} + size > std::uint64_t{1} << 32)
  {
    throw config_error(
        "watch", 0, std::format("{:#010x}", at),
        std::format("a non-empty range within the 32-bit address space, not {} bytes", size));
  }
  auto callable = detail::held_bare(std::move(handler));
  auto handle   = access([&](core::machine& target) {
    return target.add_watch(at, size, kind, [callable](core::watch_hit const& hit) {
      detail::gil_entry const locked;
      return nb::cast<core::watch_action>(detail::call_python([&] { return (*callable)(hit); }));
    });
  });
  return hook{std::move(handle), slot_};
}
}
