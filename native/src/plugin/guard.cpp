#include "guard.hpp"

#include <popcorn/core/errors.hpp>

#include <cstdint>
#include <format>
#include <string>

namespace popcorn::plugin::guard
{
namespace
{
enum class phase : std::uint8_t
{
  idle,
  constructors,
  init,
};

struct record
{
  bool             in_load = false;
  phase            now     = phase::idle;
  std::string      path;
  std::string_view violation;
};

thread_local record current;
}

loading::loading() noexcept
{
  current.in_load = true;
}

loading::~loading()
{
  current = {};
}

auto in_load() noexcept -> bool
{
  return current.in_load;
}

void watch_constructors(std::string_view path)
{
  current.path = path;
  current.now  = phase::constructors;
}

void watch_init(std::string_view path)
{
  current.path = path;
  current.now  = phase::init;
}

void violate(std::string_view call) noexcept
{
  if(current.violation.empty())
  {
    current.violation = call;
  }
}

auto violation() noexcept -> std::string_view
{
  return current.violation;
}

auto refuses(std::string_view call) -> bool
{
  if(current.now == phase::idle)
  {
    return false;
  }
  violate(call);
  if(current.now == phase::init)
  {
    throw config_error(
        current.path, 0, "popcorn_plugin_init",
        std::format(
            "no {} from a declared plugin, its models go through registrar::add_model", call));
  }
  return true;
}
}
