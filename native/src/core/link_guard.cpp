#include <popcorn/core/link_guard.hpp>

namespace popcorn::core::detail
{
constinit thread_local int link_locks_held = 0;
}
