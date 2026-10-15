#include "edited_tag.hpp"

#include <popcorn/plugin/plugin.hpp>

FIXTURE_TAG_AND_NAME(EDITED_ABI_TAG(__VERSION__, "edited-sdk-digest"), "sdk_mismatch");

extern "C" [[gnu::visibility("default")]] void popcorn_plugin_init(popcorn::plugin::registrar&)
{}
