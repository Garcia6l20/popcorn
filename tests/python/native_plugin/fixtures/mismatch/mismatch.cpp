#include <popcorn/plugin/plugin.hpp>

#include <cstdlib>
#include <fstream>

extern "C" [[gnu::visibility("default"), gnu::used, gnu::section(".popcorn_abi")]]
char const popcorn_plugin_abi_tag[] = "popcorn 0.0.0;gcc edited";
extern "C" [[gnu::visibility("default")]] char const popcorn_plugin_name[] = "mismatch";

extern "C" [[gnu::visibility("default")]] void popcorn_plugin_init(popcorn::plugin::registrar&)
{}

namespace
{
struct marks
{
  marks()
  {
    if(auto const* const path = std::getenv("MISMATCH_MARKER"))
    {
      std::ofstream{path} << "constructed\n";
    }
  }
};

marks const on_load;
}
