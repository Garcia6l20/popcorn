#include <popcorn/plugin/plugin.hpp>

POPCORN_PLUGIN(nobind, r)
{
  static_cast<void>(r);
}
