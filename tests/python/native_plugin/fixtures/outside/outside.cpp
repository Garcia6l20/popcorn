#include <popcorn/plugin/plugin.hpp>

#include <outside.hpp>

POPCORN_PLUGIN(outside, r)
{
  static_cast<void>(r);
  static_cast<void>(outside_value);
}
