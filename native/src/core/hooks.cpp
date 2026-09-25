#include "machine_impl.hpp"

#include <popcorn/core/errors.hpp>
#include <popcorn/core/intercept.hpp>

#include <utility>

namespace popcorn::core
{
hook_handle::hook_handle(std::weak_ptr<hook_registry> registry, std::uint64_t id) noexcept
    : registry_{std::move(registry)}, id_{id}
{}

hook_handle::hook_handle(hook_handle&& other) noexcept
    : registry_{std::exchange(other.registry_, {})}, id_{std::exchange(other.id_, 0)}
{}

auto hook_handle::operator=(hook_handle&& other) noexcept -> hook_handle&
{
  if(this != &other)
  {
    release();
    registry_ = std::exchange(other.registry_, {});
    id_       = std::exchange(other.id_, 0);
  }
  return *this;
}

hook_handle::~hook_handle()
{
  release();
}

void hook_handle::reset()
{
  if(auto const registry = registry_.lock(); registry and registry->running)
  {
    throw machine_running("a hook cannot be removed while a slice runs");
  }
  release();
}

void hook_handle::release() noexcept
{
  if(auto const registry = registry_.lock())
  {
    registry->remove(id_);
  }
  registry_.reset();
  id_ = 0;
}

auto hook_handle::active() const noexcept -> bool
{
  return not registry_.expired();
}

intercept_call::intercept_call(
    machine&      target,
    address       function,
    address       return_address,
    std::uint64_t occurrence) noexcept
    : machine_{&target}, function_{function}, return_address_{return_address},
      occurrence_{occurrence}
{}

auto intercept_call::function() const noexcept -> address
{
  return function_;
}

auto intercept_call::return_address() const noexcept -> address
{
  return return_address_;
}

auto intercept_call::occurrence() const noexcept -> std::uint64_t
{
  return occurrence_;
}

auto intercept_call::args(std::span<arch::value_kind const>) const -> std::vector<std::uint64_t>
{
  throw not_implemented("intercept_call::args");
}

auto intercept_call::target() const noexcept -> machine&
{
  return *machine_;
}
}
