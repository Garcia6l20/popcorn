#include "hooks.hpp"

#include "machine.hpp"
#include "python_callable.hpp"

#include <popcorn/core/errors.hpp>

#include <nanobind/nanobind.h>
#include <nanobind/stl/optional.h>
#include <nanobind/stl/vector.h>

#include <format>
#include <utility>

namespace popcorn::bind
{
namespace
{
namespace nb = reflex::py::nb;
using detail::held;

struct view_scope
{
  view_scope()                                     = default;
  view_scope(view_scope const&)                    = delete;
  auto operator=(view_scope const&) -> view_scope& = delete;
  ~view_scope()
  {
    *alive = false;
  }

  std::shared_ptr<bool> alive = std::make_shared<bool>(true);
};
}

hook::hook(core::hook_handle handle) noexcept : handle_{std::move(handle)}
{}

void hook::remove()
{
  handle_.reset();
}

auto hook::active() const noexcept -> bool
{
  return handle_.active();
}

call_view::call_view(core::intercept_call const& call, std::shared_ptr<bool> alive) noexcept
    : call_{&call}, alive_{std::move(alive)}
{}

auto call_view::call() const -> core::intercept_call const&
{
  if(not *alive_)
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
  auto callable = held(std::move(handler));
  auto handle   = machine_->add_intercept(
      at,
      [callable](core::intercept_call const& call) {
        nb::gil_scoped_acquire locked;
        view_scope const       scope;
        return nb::cast<core::intercept_result>(callable->fn(call_view{call, scope.alive}));
      },
      once ? core::intercept_mode::once : core::intercept_mode::every);
  return hook{std::move(handle)};
}

auto machine_handle::add_return_hook(
    address                              at,
    std::vector<arch::value_kind> const& args,
    arch::value_kind                     returns,
    nb::object                           handler) -> hook
{
  auto callable = held(std::move(handler));
  auto handle =
      machine_->add_return_hook(at, args, returns, [callable](core::returned_call const& done) {
        nb::gil_scoped_acquire locked;
        callable->fn(
            returned_record{
                .function     = done.function,
                .occurrence   = done.occurrence,
                .args         = done.args,
                .value        = done.value,
                .instructions = done.instructions.value,
            });
      });
  return hook{std::move(handle)};
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
  auto callable = held(std::move(handler));
  auto handle   = machine_->add_watch(at, size, kind, [callable](core::watch_hit const& hit) {
    nb::gil_scoped_acquire locked;
    return nb::cast<core::watch_action>(callable->fn(hit));
  });
  return hook{std::move(handle)};
}
}
