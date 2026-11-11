#include <popcorn/core/version.hpp>
#include <popcorn_version_string.hpp>

namespace popcorn::core
{
auto version() -> std::string_view
{
  return POPCORN_VERSION_STRING " (unicorn " POPCORN_UNICORN_COMMIT ")";
}
}
