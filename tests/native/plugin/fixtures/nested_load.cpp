#include <popcorn/plugin/plugin.hpp>

#include <cstdlib>

POPCORN_PLUGIN(nested_load, r)
{
  static_cast<void>(r);
  static_cast<void>(popcorn::plugin::load(
      std::getenv("POPCORN_PLUGIN_NESTED"), popcorn::plugin::scope::declared));
}
