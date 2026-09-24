#include <popcorn/core/fwd.hpp>

#include <doctest/doctest.h>

#include <type_traits>

TEST_CASE("core forward declarations name classes")
{
  CHECK(std::is_class_v<popcorn::core::clock>);
  CHECK(std::is_class_v<popcorn::core::scheduler>);
  CHECK(std::is_class_v<popcorn::core::event_port>);
  CHECK(std::is_class_v<popcorn::core::machine_services>);
}
