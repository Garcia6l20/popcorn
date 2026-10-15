#include "edited_tag.hpp"

#include <popcorn/plugin/plugin.hpp>

FIXTURE_TAG_AND_NAME(EDITED_ABI_TAG("0.0.0 edited", POPCORN_SDK_DIGEST), "mismatch");

extern "C" [[gnu::visibility("default")]] void popcorn_plugin_init(popcorn::plugin::registrar&)
{}
