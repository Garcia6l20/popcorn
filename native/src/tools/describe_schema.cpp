#include <popcorn/core/description.hpp>

#include <print>

auto main() -> int
{
  std::println("{}", popcorn::description_json_schema());
}
