#include <popcorn/periph/stm32/builtin.hpp>
#include <popcorn/plugin/plugin.hpp>

extern "C" [[gnu::visibility("default"), gnu::used, gnu::section(".popcorn_abi")]]
char const popcorn_plugin_abi_tag[] = POPCORN_ABI_TAG_LITERAL;

extern "C" [[gnu::visibility("default")]] void popcorn_plugin_init(popcorn::plugin::registrar&)
{
  popcorn::periph::stm32::register_builtin_models();
}
