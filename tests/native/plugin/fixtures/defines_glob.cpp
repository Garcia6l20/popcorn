#include <popcorn/periph/model.hpp>
#include <popcorn/plugin/plugin.hpp>

#include <optional>
#include <string>
#include <string_view>

auto popcorn::periph::registered_glob(std::string_view, std::string_view)
    -> std::optional<std::string>
{
  return std::nullopt;
}

POPCORN_PLUGIN(defines_glob, r)
{
  static_cast<void>(r);
}
