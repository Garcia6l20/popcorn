#include "edited_tag.hpp"

#include <popcorn/periph/stm32/builtin.hpp>
#include <popcorn/plugin/plugin.hpp>

#include <atomic>

FIXTURE_TAG_AND_NAME(POPCORN_ABI_TAG_LITERAL, "no_init_stm32");

extern "C" std::atomic<int> good_init_calls;

extern "C" [[gnu::visibility("default")]] auto no_init_stm32_dependencies() -> int
{
  popcorn::periph::stm32::register_builtin_models();
  return good_init_calls.load();
}
