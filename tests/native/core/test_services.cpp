#include <popcorn/core/services.hpp>

#include "../fake_services.hpp"

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <format>
#include <type_traits>

static_assert(std::is_abstract_v<popcorn::core::machine_services>);
static_assert(not std::is_copy_constructible_v<popcorn::core::machine_services>);
static_assert(not std::is_move_constructible_v<popcorn::core::machine_services>);

TEST_CASE("services forward the deferred requests")
{
  popcorn::testing::fake_services services;
  services.request_stop(popcorn::stop_reason::user, "model asked");
  REQUIRE(services.stop.has_value());
  CHECK(services.stop->first == popcorn::stop_reason::user);
  CHECK(services.stop->second == "model asked");

  services.request_reset(popcorn::reset_kind::watchdog);
  CHECK(services.reset == popcorn::reset_kind::watchdog);

  services.end_slice();
  services.end_slice();
  CHECK(services.slice_ends == 2);
}

TEST_CASE("services forward bus accesses and their errors")
{
  popcorn::testing::fake_services services;
  services.read_only_below = 0x2000'0000;
  CHECK(services.write(0x2000'0000, 4, 0x1234'5678, popcorn::initiator::dma).has_value());
  CHECK(services.last_by == popcorn::initiator::dma);
  CHECK(services.read(0x2000'0000, 4, popcorn::initiator::cpu) == 0x1234'5678u);
  CHECK(services.read(0x2000'0002, 2, popcorn::initiator::cpu) == 0x1234u);
  CHECK(services.last_by == popcorn::initiator::cpu);

  auto const unmapped = services.read(0x3000'0000, 1, popcorn::initiator::debugger);
  REQUIRE_FALSE(unmapped.has_value());
  CHECK(unmapped.error() == popcorn::bus_error::unmapped);

  auto const denied = services.write(0x0800'0000, 1, 0xff, popcorn::initiator::dma);
  REQUIRE_FALSE(denied.has_value());
  CHECK(std::format("{}", denied.error()) == "denied");
}

TEST_CASE("block accesses allow unaligned addresses and sizes")
{
  popcorn::testing::fake_services services;
  std::array const                in{std::byte{1}, std::byte{2}, std::byte{3}};
  CHECK(services.write_block(0x2000'0001, in, popcorn::initiator::dma).has_value());
  std::array<std::byte, 3> out{};
  CHECK(services.read_block(0x2000'0001, out, popcorn::initiator::debugger).has_value());
  CHECK(out == in);
  CHECK(services.read(0x2000'0002, 2, popcorn::initiator::cpu) == 0x0302u);

  std::array<std::byte, 4> past{};
  auto const partial = services.read_block(0x2000'0002, past, popcorn::initiator::cpu);
  REQUIRE_FALSE(partial.has_value());
  CHECK(partial.error() == popcorn::core::block_error{popcorn::bus_error::unmapped, 0x2000'0004});
  CHECK(past[0] == std::byte{2});
  CHECK(past[1] == std::byte{3});

  services.read_only_below = 0x2000'0000;
  std::array<std::byte, 6> const spanning{};
  auto const refused = services.write_block(0x1fff'fffe, spanning, popcorn::initiator::dma);
  REQUIRE_FALSE(refused.has_value());
  CHECK(refused.error().at == 0x1fff'fffe);
  CHECK(refused.error().error == popcorn::bus_error::denied);
}

TEST_CASE("blocks split into the largest naturally aligned accesses")
{
  static_assert(popcorn::core::block_access_size(0x2000'0000, 8) == 4);
  static_assert(popcorn::core::block_access_size(0x2000'0000, 3) == 2);
  static_assert(popcorn::core::block_access_size(0x2000'0001, 8) == 1);
  static_assert(popcorn::core::block_access_size(0x2000'0002, 8) == 2);
  static_assert(popcorn::core::block_access_size(0x2000'0004, 1) == 1);
  CHECK(popcorn::core::block_access_size(0x2000'0006, 2) == 2);
}

TEST_CASE("access values and blocks fit their bounds")
{
  static_assert(popcorn::core::fits_access(0xff, 1));
  static_assert(not popcorn::core::fits_access(0x100, 1));
  static_assert(popcorn::core::fits_access(0xffff, 2));
  static_assert(not popcorn::core::fits_access(0x1'0000, 2));
  static_assert(popcorn::core::fits_access(0xffff'ffff, 4));
  static_assert(popcorn::core::fits_address_space(0xffff'fffc, 4));
  static_assert(not popcorn::core::fits_address_space(0xffff'fffc, 5));
  static_assert(popcorn::core::fits_address_space(0, 0x1'0000'0000));
  CHECK(popcorn::core::fits_access(0, 1));
}

TEST_CASE("enumerator checks come from reflection")
{
  static_assert(popcorn::is_enumerator(popcorn::reset_kind::option_load));
  static_assert(not popcorn::is_enumerator(static_cast<popcorn::reset_kind>(200)));
  CHECK(std::format("{}", popcorn::reset_kind::window_watchdog) == "window_watchdog");
}
