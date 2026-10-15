#include <popcorn/plugin/plugin.hpp>

#include <cstdlib>

POPCORN_PLUGIN(static_core_hidden, r)
{
  static_cast<void>(r);
}

namespace
{
struct aborts
{
  aborts()
  {
    std::abort();
  }
};

aborts const on_load;
}
