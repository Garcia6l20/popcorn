#include "fixture_files.hpp"

#include <popcorn/plugin/plugin.hpp>

#include <doctest/doctest.h>

TEST_CASE("a plugin record stays valid while more plugins load")
{
  plugin_test::copies files;
  auto const& first = popcorn::plugin::load(files.of("good"), popcorn::plugin::scope::declared);
  for(int copy = 0; copy < 10; ++copy)
  {
    static_cast<void>(popcorn::plugin::load(files.of("good"), popcorn::plugin::scope::declared));
  }
  CHECK(first.name == "good");
  CHECK(first.models.size() == 1);
}
