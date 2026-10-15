#include <popcorn/arch/armv8m/armv8m.hpp>
#include <popcorn/plugin/plugin.hpp>

POPCORN_PLUGIN(armv8m, r)
{
  static_cast<void>(r);
  popcorn::arch::armv8m::register_builtin();
}
