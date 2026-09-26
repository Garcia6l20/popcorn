#include "../register_layer.hpp"

#include <popcorn/periph/model.hpp>
#include <popcorn/periph/stm32/board.hpp>
#include <popcorn/periph/stm32/builtin.hpp>

#include <array>
#include <mutex>
#include <string_view>

namespace popcorn::periph::stm32
{
namespace
{
struct builtin
{
  std::string_view ip;
  std::string_view ip_version_glob;
  auto (*make)(board const& b) -> model_factory;
};

constexpr std::array<builtin, 0> builtins{};

auto default_board() -> board const&
{
  static board const instance{};
  return instance;
}
}

void register_builtin_models()
{
  static std::once_flag once;
  std::call_once(once, [] {
    for(auto const& entry : builtins)
    {
      register_model(entry.ip, entry.ip_version_glob, entry.make(default_board()));
    }
  });
}

auto factories(board const& b, mcu_description const& part)
    -> std::map<std::string, model_factory, std::less<>>
{
  std::map<std::string, model_factory, std::less<>> out;
  for(auto const& unit : part.peripherals)
  {
    if(unit.alias_of)
    {
      continue;
    }
    for(auto const& entry : builtins)
    {
      if(entry.ip == unit.ip and glob_match(entry.ip_version_glob, unit.ip_version))
      {
        out.emplace(unit.name, entry.make(b));
        break;
      }
    }
  }
  return out;
}
}
