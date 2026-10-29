#include <popcorn/core/snapshot.hpp>

#include <popcorn/core/errors.hpp>

#include <reflex/serde/bson.hpp>

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <tuple>
#include <vector>

namespace
{
namespace core = popcorn::core;

auto filled() -> core::snapshot_data
{
  core::snapshot_data data;
  data.format             = core::snapshot_format_version;
  data.description_digest = "d1";
  data.image_digests      = {"i1", "i2"};
  data.popcorn_version    = "0.1.0.dev0";
  data.unicorn_commit     = "938efd13aeab";
  data.generation         = 3;
  data.memory             = {
      {.region = "SRAM1",   .start = 0x2000'0000, .bytes = {std::byte{1}, std::byte{2}}},
      {.region = "BKPSRAM", .start = 0x4003'6400, .bytes = {}                          },
  };
  data.registers = {
      {.name = "r0", .value = 7                    },
      {.name = "d0", .value = 0x4000'0000'0000'0000}
  };
  data.arch   = {.owner = "arch", .data = {std::byte{0xaa}}};
  data.models = {
      {.owner = "UART1", .data = {std::byte{0x10}, std::byte{0x20}}}
  };
  data.peripherals = {
      {.peripheral   = "UART1",
       .words        = {0, 0xc0, 0xffff'ffff},
       .irq_levels   = {1},
       .dma_levels   = {0, 1},
       .write_locked = {0x5, 0x8000'0000}}
  };
  data.events = {
      .pending =
          {{.at = popcorn::icount{100}, .seq = 4, .id = {.owner = 0, .kind = 2}, .payload = 9},
                    {.at      = popcorn::icount{200},
            .seq     = 5,
            .id      = {.owner = 0xffff'ffff, .kind = 1},
            .payload = 0}},
      .next_seq = 6,
  };
  data.clock.now             = popcorn::icount{150};
  data.clock.epoch_icount    = popcorn::icount{50};
  data.clock.epoch_time      = popcorn::vtime{12'500'000};
  data.clock.before_reset_ps = 7'000'000'000;
  data.clock.core_hz         = 4'000'000;
  data.clock.cpi             = 1.5;
  data.clock.domain_names    = {"apb1", "apb2"};
  data.clock.domain_hz       = {4'000'000, 0};
  data.intercepts            = {
      {.at = 0x0800'0100, .calls = 12, .spent = true}
  };
  data.return_hooks = {
      {.at        = 0x0800'0300,
       .calls     = 2,
       .in_flight = {
           {.return_address = 0x0800'0401,
            .stack_pointer  = 0x2000'7ff0,
            .args           = {1, 2},
            .entered        = popcorn::icount{140}}}}
  };
  return data;
}
}

TEST_CASE("snapshot data is plain comparable data")
{
  auto const data = filled();
  auto       copy = data;
  CHECK(copy == data);
  copy.events.pending.back().seq = 7;
  CHECK(copy != data);
  copy                        = data;
  copy.clock.domain_hz.back() = 1;
  CHECK(copy != data);
}

TEST_CASE("the saved form round-trips to an equal value")
{
  auto const data  = filled();
  auto const bytes = core::encode_snapshot(data);
  auto const back  = core::decode_snapshot(bytes);
  CHECK(back == data);
  CHECK(back.memory == data.memory);
  CHECK(back.registers == data.registers);
  CHECK(back.arch == data.arch);
  CHECK(back.peripherals == data.peripherals);
  CHECK(back.peripherals.front().write_locked == std::vector<std::uint32_t>{0x5, 0x8000'0000});
  CHECK(back.models == data.models);
  CHECK(back.clock == data.clock);
  CHECK(back.events == data.events);
}

TEST_CASE("a corrupted saved form is a config_error")
{
  std::vector<std::byte> const truncated{std::byte{5}, std::byte{0}, std::byte{0}, std::byte{0}};
  CHECK_THROWS_AS(std::ignore = popcorn::core::decode_snapshot(truncated), popcorn::config_error);

  auto bytes = core::encode_snapshot(filled());
  CHECK(bytes.size() > 4);
  bytes[bytes.size() - 1] = std::byte{0xff};
  CHECK_THROWS_AS(std::ignore = popcorn::core::decode_snapshot(bytes), popcorn::config_error);
}

TEST_CASE("a saved form with a future format is a config_error")
{
  auto data        = filled();
  data.format      = core::snapshot_format_version + 1;
  auto const bytes = core::encode_snapshot(data);
  CHECK_THROWS_AS(std::ignore = popcorn::core::decode_snapshot(bytes), popcorn::config_error);
}

TEST_CASE("a saved form of format 8 is a config_error naming the format")
{
  auto data   = filled();
  data.format = 8;
  CHECK_THROWS_WITH_AS(
      std::ignore = core::decode_snapshot(core::encode_snapshot(data)),
      doctest::Contains("format, expected snapshot_format_version 9, bytes have 8"),
      popcorn::config_error);
}

TEST_CASE("a saved form of another format with a different member set names the format")
{
  struct reshaped
  {
    std::uint32_t format{};
    std::uint32_t gone{};
  };
  std::vector<std::byte> bytes;
  reflex::serde::bson::serializer{bytes}.dump(reshaped{.format = 7, .gone = 1});
  CHECK_THROWS_WITH_AS(
      std::ignore = core::decode_snapshot(bytes), doctest::Contains("format, expected"),
      popcorn::config_error);
}

TEST_CASE("a saved form that does not lead with the format element is a config_error on bytes")
{
  struct unordered
  {
    std::uint32_t gone{};
    std::uint32_t format{};
  };
  std::vector<std::byte> bytes;
  reflex::serde::bson::serializer{bytes}.dump(unordered{.gone = 1, .format = 8});
  CHECK_THROWS_WITH_AS(
      std::ignore = core::decode_snapshot(bytes), doctest::Contains("bytes"),
      popcorn::config_error);
}

TEST_CASE("a saved form whose leading format is a 32-bit integer is a config_error on bytes")
{
  struct narrow
  {
    std::int32_t format{};
    std::int32_t after{};
  };
  std::vector<std::byte> bytes;
  reflex::serde::bson::serializer{bytes}.dump(narrow{.format = 8, .after = 0});
  CHECK_THROWS_WITH_AS(
      std::ignore = core::decode_snapshot(bytes),
      doctest::Contains("bytes, expected a snapshot matching snapshot_data: no leading format"),
      popcorn::config_error);
}

TEST_CASE("an NMI line level survives the saved form")
{
  auto data                          = filled();
  data.peripherals.front().nmi_level = 1;
  auto const back                    = core::decode_snapshot(core::encode_snapshot(data));
  CHECK(back.peripherals.front().nmi_level == 1);
  CHECK(back == data);
  CHECK(back != filled());
}

TEST_CASE("an event's lane survives the saved form")
{
  auto data                          = filled();
  data.events.pending.front().lane   = 3;
  data.events.pending.front().kind   = core::deadline::time;
  data.events.pending.front().due_ps = 25'000'000;
  auto const back                    = core::decode_snapshot(core::encode_snapshot(data));
  CHECK(back.events.pending.front().lane == 3);
  CHECK(back == data);
}

TEST_CASE("the clock hold survives the saved form")
{
  auto data            = filled();
  data.clock.held      = popcorn::icount{170};
  data.clock.held_time = popcorn::vtime{17'500'000};
  CHECK(core::decode_snapshot(core::encode_snapshot(data)) == data);
}

TEST_CASE("time-keyed events survive the saved form")
{
  auto data                          = filled();
  data.events.pending.front().kind   = core::deadline::time;
  data.events.pending.front().due_ps = 123'456'789;
  CHECK(core::decode_snapshot(core::encode_snapshot(data)) == data);
}

TEST_CASE("a snapshot read back from bytes has no engine context")
{
  core::snapshot const read_back{filled()};
  CHECK_FALSE(read_back.has_engine_context());
  CHECK(read_back.machine_id() == 0);
  CHECK(read_back.data() == filled());
  core::snapshot const copy = read_back;
  CHECK(copy.data().generation == 3);
}

TEST_CASE("a peripheral held in reset survives the saved form")
{
  auto data                              = filled();
  data.peripherals.front().held_in_reset = 1;
  auto const back                        = core::decode_snapshot(core::encode_snapshot(data));
  CHECK(back.peripherals.front().held_in_reset == 1);
  CHECK(back == data);
}

TEST_CASE("a pending reset request survives the saved form")
{
  auto data          = filled();
  data.reset_pending = popcorn::reset_kind::watchdog;
  auto const back    = core::decode_snapshot(core::encode_snapshot(data));
  CHECK(back.reset_pending == popcorn::reset_kind::watchdog);
  CHECK(back == data);
  CHECK(back != filled());
}
