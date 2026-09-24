#include <popcorn/core/version.hpp>

namespace popcorn::core
{
auto version() -> std::string_view
{
  return POPCORN_VERSION " (unicorn " POPCORN_UNICORN_COMMIT ")";
}
}
