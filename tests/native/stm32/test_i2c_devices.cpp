#include <popcorn/core/errors.hpp>
#include <popcorn/periph/devices/i2c_eeprom.hpp>
#include <popcorn/periph/devices/i2c_table.hpp>

#include <doctest/doctest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <initializer_list>
#include <vector>

#include <unistd.h>

namespace
{
namespace devices = popcorn::periph::devices;
using popcorn::reset_kind;
using popcorn::vtime;

constexpr std::uint64_t us = 1'000'000;
constexpr std::uint64_t ms = 1'000 * us;

auto small_eeprom() -> devices::eeprom_24xx_options
{
  return {.address = 0x50, .size = 256, .address_bytes = 1, .page_size = 16};
}

struct host
{
  popcorn::periph::stm32::i2c_target& target;
  std::uint64_t                       now{us};

  auto write(std::initializer_list<std::uint8_t> bytes) -> std::vector<bool>
  {
    std::vector<bool> acks;
    now += 25 * us;
    acks.push_back(target.start(false, vtime{now}));
    if(not acks.back())
    {
      target.stop(vtime{now += us});
      return acks;
    }
    for(auto const byte : bytes)
    {
      now += 25 * us;
      acks.push_back(target.write(byte, vtime{now}));
    }
    target.stop(vtime{now += us});
    return acks;
  }

  auto read(std::size_t count, bool stop = true) -> std::vector<std::uint8_t>
  {
    std::vector<std::uint8_t> out;
    now += 25 * us;
    REQUIRE(target.start(true, vtime{now}));
    for(std::size_t index = 0; index < count; ++index)
    {
      now += 25 * us;
      out.push_back(target.read(vtime{now}));
    }
    if(stop)
    {
      target.stop(vtime{now += us});
    }
    return out;
  }

  auto random_read(std::initializer_list<std::uint8_t> address, std::size_t count)
      -> std::vector<std::uint8_t>
  {
    now += 25 * us;
    REQUIRE(target.start(false, vtime{now}));
    for(auto const byte : address)
    {
      now += 25 * us;
      REQUIRE(target.write(byte, vtime{now}));
    }
    return read(count);
  }
};

auto byte_at(devices::eeprom_24xx const& eeprom, std::size_t at) -> std::uint32_t
{
  return std::to_integer<std::uint32_t>(eeprom.contents()[at]);
}
}

TEST_CASE("a page write wraps within its page and reads back sequentially")
{
  devices::eeprom_24xx eeprom{small_eeprom()};
  host                 h{eeprom};
  CHECK(eeprom.address() == 0x50);
  CHECK(h.write({0x1C, 1, 2, 3, 4, 5, 6}) == std::vector<bool>(8, true));
  CHECK(byte_at(eeprom, 0x1C) == 1);
  CHECK(byte_at(eeprom, 0x1F) == 4);
  CHECK(byte_at(eeprom, 0x10) == 5);
  CHECK(byte_at(eeprom, 0x11) == 6);
  CHECK(byte_at(eeprom, 0x20) == 0xFF);
  h.now += 10 * ms;
  CHECK(h.random_read({0x1E}, 4) == std::vector<std::uint8_t>{3, 4, 0xFF, 0xFF});
}

TEST_CASE("the write cycle NACKs the address until tWR has passed, to the virtual ns")
{
  devices::eeprom_24xx eeprom{small_eeprom()};
  host                 h{eeprom};
  REQUIRE(h.write({0x00, 0xAA}) == std::vector<bool>(3, true));
  auto const stop = h.now;
  CHECK_FALSE(eeprom.start(false, vtime{stop + 5 * ms - 1}));
  eeprom.stop(vtime{stop + 5 * ms - 1});
  CHECK_FALSE(eeprom.start(true, vtime{stop + 5 * ms - 1}));
  eeprom.stop(vtime{stop + 5 * ms - 1});
  CHECK(eeprom.start(false, vtime{stop + 5 * ms}));
  eeprom.stop(vtime{stop + 5 * ms});
}

TEST_CASE("a write of the word address only starts no write cycle")
{
  devices::eeprom_24xx eeprom{small_eeprom()};
  host                 h{eeprom};
  REQUIRE(h.write({0x40}) == std::vector<bool>(2, true));
  CHECK(eeprom.start(true, vtime{h.now}));
  eeprom.stop(vtime{h.now});
}

TEST_CASE("a repeated START discards the data of an unfinished write")
{
  devices::eeprom_24xx eeprom{small_eeprom()};
  host                 h{eeprom};
  REQUIRE(eeprom.start(false, vtime{h.now}));
  REQUIRE(eeprom.write(0x30, vtime{h.now += us}));
  REQUIRE(eeprom.write(0x55, vtime{h.now += us}));
  CHECK(h.read(1) == std::vector<std::uint8_t>{0xFF});
  CHECK(byte_at(eeprom, 0x30) == 0xFF);
  CHECK(eeprom.start(false, vtime{h.now += us}));
  eeprom.stop(vtime{h.now});
}

TEST_CASE("a sequential read across the end of the array wraps to 0")
{
  devices::eeprom_24xx eeprom{small_eeprom()};
  host                 h{eeprom};
  REQUIRE(h.write({0x00, 0x11}) == std::vector<bool>(3, true));
  h.now += 5 * ms;
  CHECK(h.random_read({0xFE}, 3) == std::vector<std::uint8_t>{0xFF, 0xFF, 0x11});
}

TEST_CASE("two word address bytes, most significant first")
{
  devices::eeprom_24xx eeprom{
      {.size = 4096, .page_size = 32}
  };
  host h{eeprom};
  REQUIRE(h.write({0x0A, 0xBC, 0x42}) == std::vector<bool>(4, true));
  CHECK(byte_at(eeprom, 0x0ABC) == 0x42);
  h.now += 5 * ms;
  CHECK(h.random_read({0x0A, 0xBC}, 1) == std::vector<std::uint8_t>{0x42});
}

TEST_CASE("save keeps the changed pages and the write cycle, load restores both")
{
  devices::eeprom_24xx eeprom;
  host                 h{eeprom};
  REQUIRE(h.write({0x00, 0x20, 7, 8}) == std::vector<bool>(5, true));
  std::vector<std::byte> saved;
  eeprom.save(saved);
  CHECK(saved.size() < 512);

  devices::eeprom_24xx other;
  other.load(saved);
  CHECK(byte_at(other, 0x20) == 7);
  CHECK(byte_at(other, 0x21) == 8);
  CHECK(byte_at(other, 0x22) == 0xFF);
  CHECK_FALSE(other.start(false, vtime{h.now + 5 * ms - 1}));
  CHECK(other.start(true, vtime{h.now + 5 * ms}));
  CHECK(other.read(vtime{h.now + 5 * ms}) == 0xFF);

  devices::eeprom_24xx bigger{{.size = 65536}};
  CHECK_THROWS_AS(bigger.load(saved), popcorn::config_error);
}

TEST_CASE("an image file gives the initial contents")
{
  auto const path = std::filesystem::temp_directory_path()
                  / std::format("popcorn-test-i2c-eeprom-{}.bin", ::getpid());
  {
    std::ofstream file{path, std::ios::binary};
    file << "abc";
  }
  auto options  = small_eeprom();
  options.image = path;
  devices::eeprom_24xx eeprom{options};
  CHECK(byte_at(eeprom, 0) == 'a');
  CHECK(byte_at(eeprom, 2) == 'c');
  CHECK(byte_at(eeprom, 3) == 0xFF);
  std::filesystem::remove(path);
}

TEST_CASE("inconsistent EEPROM options are refused")
{
  auto const make = [](devices::eeprom_24xx_options options) {
    return devices::eeprom_24xx{options};
  };
  auto options = small_eeprom();
  options.size = 512;
  CHECK_THROWS_AS(make(options), popcorn::config_error);
  options           = small_eeprom();
  options.page_size = 12;
  options.size      = 192;
  CHECK_THROWS_AS(make(options), popcorn::config_error);
  options               = small_eeprom();
  options.address_bytes = 3;
  CHECK_THROWS_AS(make(options), popcorn::config_error);
  options         = small_eeprom();
  options.address = 0x80;
  CHECK_THROWS_AS(make(options), popcorn::config_error);
  options      = small_eeprom();
  options.size = 0;
  CHECK_THROWS_AS(make(options), popcorn::config_error);
}

TEST_CASE("a table target records writes in order and replies from its table")
{
  devices::table_target table{
      {.address = 0x38, .register_bytes = 2, .replies = {{0x4002, {0x12, 0x34}}}}
  };
  host h{table};
  CHECK(table.address() == 0x38);
  CHECK(h.write({0x40, 0x00, 0x01}) == std::vector<bool>(4, true));
  CHECK(h.write({0x40, 0x01, 0x02, 0x03}) == std::vector<bool>(5, true));
  CHECK(h.random_read({0x40, 0x02}, 3) == std::vector<std::uint8_t>{0x12, 0x34, 0xFF});
  CHECK(h.random_read({0x40, 0x07}, 1) == std::vector<std::uint8_t>{0xFF});
  REQUIRE(table.writes().size() == 2);
  CHECK(table.writes()[0] == devices::table_write{.reg = 0x4000, .bytes = {0x01}});
  CHECK(
      table.writes()[1]
      == devices::table_write{
          .reg = 0x4001, .bytes = {0x02, 0x03}
  });
}

TEST_CASE("a table target can refuse reads of registers without a reply")
{
  devices::table_target table{
      {.address = 0x38, .replies = {{0x10, {0xAB}}}, .nack_unknown = true}
  };
  host h{table};
  CHECK(h.random_read({0x10}, 1) == std::vector<std::uint8_t>{0xAB});
  REQUIRE(table.start(false, vtime{h.now += us}));
  REQUIRE(table.write(0x11, vtime{h.now += us}));
  CHECK_FALSE(table.start(true, vtime{h.now += us}));
  table.stop(vtime{h.now += us});
  CHECK(table.writes().empty());
}

TEST_CASE("a table target saves and loads its recorded writes and the transfer in progress")
{
  devices::table_target_options const options{.address = 0x38, .replies = {{0x05, {1, 2, 3}}}};
  devices::table_target               table{options};
  host                                h{table};
  REQUIRE(h.write({0x01, 0x99}) == std::vector<bool>(3, true));
  REQUIRE(h.random_read({0x05}, 1) == std::vector<std::uint8_t>{1});
  REQUIRE(table.start(true, vtime{h.now += us}));
  REQUIRE(table.read(vtime{h.now += us}) == 1);
  std::vector<std::byte> saved;
  table.save(saved);

  devices::table_target other{options};
  other.load(saved);
  CHECK(other.read(vtime{h.now += us}) == 2);
  REQUIRE(other.writes().size() == 1);
  CHECK(other.writes()[0] == devices::table_write{.reg = 0x01, .bytes = {0x99}});
}

TEST_CASE("table target options outside the bus limits are refused")
{
  auto const make = [](devices::table_target_options options) {
    return devices::table_target{options};
  };
  CHECK_THROWS_AS(make({.address = 0x80}), popcorn::config_error);
  CHECK_THROWS_AS(make({.address = 0x38, .register_bytes = 0}), popcorn::config_error);
  CHECK_THROWS_AS(make({.address = 0x38, .register_bytes = 5}), popcorn::config_error);
}

TEST_CASE("a repeated START ends a table write, the next one starts with its own register")
{
  devices::table_target table{{.address = 0x38}};
  REQUIRE(table.start(false, vtime{us}));
  REQUIRE(table.write(0x10, vtime{2 * us}));
  REQUIRE(table.write(0xAA, vtime{3 * us}));
  REQUIRE(table.start(false, vtime{4 * us}));
  REQUIRE(table.write(0x11, vtime{5 * us}));
  REQUIRE(table.write(0xBB, vtime{6 * us}));
  table.stop(vtime{7 * us});
  REQUIRE(table.writes().size() == 2);
  CHECK(table.writes()[0] == devices::table_write{.reg = 0x10, .bytes = {0xAA}});
  CHECK(table.writes()[1] == devices::table_write{.reg = 0x11, .bytes = {0xBB}});
}

TEST_CASE("a power-on reset during tWR lets the EEPROM answer at once in the new timeline")
{
  for(auto const kind : {reset_kind::power_on, reset_kind::brown_out})
  {
    CAPTURE(kind);
    devices::eeprom_24xx eeprom{small_eeprom()};
    host                 h{eeprom, 50 * ms};
    REQUIRE(h.write({0x00, 0x11}) == std::vector<bool>(3, true));
    h.now += 5 * ms;
    REQUIRE(h.write({0x20, 0xAA, 0xBB}) == std::vector<bool>(4, true));
    CHECK_FALSE(eeprom.start(false, vtime{h.now += us}));
    eeprom.stop(vtime{h.now += us});
    eeprom.reset(kind);
    h.now = us;
    CHECK(h.read(1) == std::vector<std::uint8_t>{0x11});
    CHECK(byte_at(eeprom, 0x20) == 0xAA);
    CHECK(byte_at(eeprom, 0x21) == 0xBB);
  }
}

TEST_CASE("a reset of another kind leaves the EEPROM in its write cycle")
{
  devices::eeprom_24xx eeprom{small_eeprom()};
  host                 h{eeprom};
  REQUIRE(h.write({0x20, 0xAA}) == std::vector<bool>(3, true));
  eeprom.reset(reset_kind::software);
  CHECK_FALSE(eeprom.start(false, vtime{h.now += us}));
  eeprom.stop(vtime{h.now += us});
}

TEST_CASE("a power-on reset drops the table write in progress and keeps the recorded ones")
{
  for(auto const kind : {reset_kind::power_on, reset_kind::software})
  {
    CAPTURE(kind);
    devices::table_target table{{.address = 0x38}};
    host                  h{table};
    REQUIRE(h.write({0x01, 0x99}) == std::vector<bool>(3, true));
    REQUIRE(table.start(false, vtime{h.now += us}));
    REQUIRE(table.write(0x02, vtime{h.now += us}));
    REQUIRE(table.write(0x77, vtime{h.now += us}));
    table.reset(kind);
    table.stop(vtime{h.now += us});
    auto const kept = kind == reset_kind::power_on ? 1u : 2u;
    REQUIRE(table.writes().size() == kept);
    CHECK(table.writes()[0] == devices::table_write{.reg = 0x01, .bytes = {0x99}});
  }
}
