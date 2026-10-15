#include <popcorn/plugin/bindings.hpp>

NB_MODULE(_native, m)
{
  popcorn::plugin::require_core(m);
}
