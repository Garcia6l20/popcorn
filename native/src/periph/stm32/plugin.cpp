#include <popcorn/periph/stm32/builtin.hpp>
#include <popcorn/plugin/plugin.hpp>

POPCORN_PLUGIN(stm32, r)
{
  static_cast<void>(r);
  popcorn::periph::stm32::register_builtin_models();
}
