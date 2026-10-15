#include "edited_tag.hpp"

namespace popcorn::plugin
{
class registrar;
}

FIXTURE_TAG_AND_NAME(POPCORN_ABI_TAG_LITERAL, "glibcxx_debug");

extern "C" [[gnu::visibility("default")]] void popcorn_plugin_init(popcorn::plugin::registrar&)
{}
