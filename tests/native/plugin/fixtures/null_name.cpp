#include "edited_tag.hpp"

#include <popcorn/plugin/plugin.hpp>

FIXTURE_TAG_AND_NAME(POPCORN_ABI_TAG_LITERAL, nullptr);

extern "C" [[gnu::visibility("default")]] void popcorn_plugin_init(popcorn::plugin::registrar&)
{}
