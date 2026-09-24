#include <popcorn/core/engine.hpp>
#include <popcorn/core/smoke.hpp>

#include <doctest/doctest.h>

#include <cstdint>
#include <cstdlib>
#include <string_view>

namespace
{
constexpr auto fnv1a(std::string_view data) -> std::uint32_t
{
  std::uint32_t hash = 0x811c9dc5u;
  for(char const c : data)
  {
    hash ^= static_cast<std::uint8_t>(c);
    hash *= 0x01000193u;
  }
  return hash;
}
}

TEST_CASE("smoke firmware reaches bkpt with its checksum in r0")
{
  char const* const elf = std::getenv("POPCORN_SMOKE_ELF");
  REQUIRE(elf != nullptr);
  auto const engine = popcorn::core::open_m33();
  CHECK(popcorn::core::run_to_bkpt(engine, elf) == fnv1a("popcorn smoke firmware"));
}
