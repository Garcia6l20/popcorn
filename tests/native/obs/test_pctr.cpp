#include <popcorn/core/description.hpp>
#include <popcorn/core/intercept.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/core/trace.hpp>
#include <popcorn/obs/errors.hpp>
#include <popcorn/obs/pctr.hpp>

#include "../support/machine_fixture.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <vector>

namespace
{
namespace core = popcorn::core;
namespace obs  = popcorn::obs;
using core::trace_kind;
using core::trace_record;
using popcorn::icount;
using popcorn::stop_reason;
using popcorn::testing::m33;
using popcorn::testing::sym;
using fixture = popcorn::testing::machine_fixture<>;

struct decoded_record
{
  trace_record               record;
  std::uint64_t              time{};
  std::optional<std::string> name;
};

struct decoded
{
  std::array<std::byte, 8>    header{};
  std::vector<decoded_record> records;
  std::vector<std::string>    strings;
  std::size_t                 file_records{};
  std::size_t                 size{};
};

auto scratch(std::string const& name) -> std::filesystem::path
{
  char const* const path = std::getenv("POPCORN_OBS_SCRATCH");
  REQUIRE(path != nullptr);
  auto const dir = std::filesystem::path{path} / "pctr";
  std::filesystem::create_directories(dir);
  return dir / name;
}

auto decode(std::filesystem::path const& path) -> decoded
{
  std::ifstream           in{path, std::ios::binary};
  std::vector<char> const chars{std::istreambuf_iterator<char>{in}, {}};
  decoded                 out;
  out.size = chars.size();
  REQUIRE(chars.size() >= 8);
  std::memcpy(out.header.data(), chars.data(), 8);
  std::map<std::uint32_t, std::string> names;
  std::size_t                          at   = 8;
  std::uint64_t                        time = 0;
  while(at < chars.size())
  {
    REQUIRE(at + core::pctr_header_size <= chars.size());
    trace_record                                record{};
    std::array<std::byte, sizeof(trace_record)> raw{};
    std::memcpy(raw.data(), chars.data() + at, core::pctr_header_size);
    record = std::bit_cast<trace_record>(raw);
    at += core::pctr_header_size;
    ++out.file_records;
    if(record.kind == trace_kind::str)
    {
      std::string text(chars.data() + at, record.aux);
      for(auto pad = at + record.aux; pad < at + record.aux + (-std::size_t{record.aux} & 3); ++pad)
      {
        REQUIRE(pad < chars.size());
        CHECK(chars[pad] == 0);
      }
      at += record.aux + (-std::size_t{record.aux} & 3);
      CHECK_FALSE(names.contains(record.dt));
      names[record.dt] = text;
      out.strings.push_back(text);
      continue;
    }
    auto const words = core::pctr_payload_words(record.kind);
    REQUIRE(at + 4 * words <= chars.size());
    for(std::size_t word = 0; word < words; ++word)
    {
      std::memcpy(&record.payload[word], chars.data() + at + 4 * word, 4);
    }
    at += 4 * words;
    time = record.kind == trace_kind::time
             ? record.payload[0] | std::uint64_t{record.payload[1]} << 32
             : time + record.dt;
    decoded_record entry{.record = record, .time = time, .name = std::nullopt};
    if(record.kind == trace_kind::mmio or record.kind == trace_kind::call)
    {
      if(record.aux != core::trace_id_overflow)
      {
        REQUIRE(names.contains(record.aux));
        entry.name = names[record.aux];
      }
    }
    out.records.push_back(entry);
  }
  CHECK(at == chars.size());
  return out;
}
}

TEST_CASE("a machine's trace: origin first, register and symbol names before their use")
{
  fixture f{m33(), "periph", {}};
  REQUIRE(f.run_to_stop().reason == stop_reason::user);
  auto const path   = scratch("periph.pctr");
  auto const origin = f.target->virtual_clock().now();
  auto       writer = obs::pctr_writer_for(*f.target, path);
  auto       sink   = f.target->add_trace_sink(
      {trace_kind::block, trace_kind::mmio, trace_kind::call, trace_kind::irq, trace_kind::stop},
      writer->sink());
  auto hook =
      f.target->add_intercept(sym("periph", "cpu_accesses"), [](core::intercept_call const&) {
        return core::intercept_result::proceed();
      });
  f.set("pc", sym("periph", "cpu_accesses"));
  REQUIRE(f.run_to_stop().reason == stop_reason::user);
  auto const end = f.target->virtual_clock().now();
  sink.reset();
  writer->close();
  writer->close();

  auto const file = decode(path);
  CHECK(
      file.header
      == std::array{
          std::byte{'P'}, std::byte{'C'}, std::byte{'T'}, std::byte{'R'}, std::byte{1},
          std::byte{0}, std::byte{0}, std::byte{0}});
  REQUIRE_FALSE(file.records.empty());
  CHECK(file.records.front().record.kind == trace_kind::time);
  CHECK(file.records.front().time == origin.value);
  CHECK(file.records.back().record.kind == trace_kind::stop);
  CHECK(file.records.back().time == end.value);
  CHECK(file.file_records == writer->records());
  CHECK(file.size == writer->bytes());
  CHECK(
      std::set<std::string>(file.strings.begin(), file.strings.end()).size()
      == file.strings.size());

  auto const&              desc = f.target->description();
  std::vector<std::string> expected;
  for(auto const& held : f.target->trace().records())
  {
    if(held.kind == trace_kind::mmio and held.payload[0] >= sym("periph", "cpu_accesses"))
    {
      auto const reg = popcorn::register_at(desc, held.aux);
      REQUIRE(reg.has_value());
      auto const& peripheral = desc.peripherals[reg->peripheral];
      expected.push_back(peripheral.name + "." + peripheral.registers[reg->reg].name);
    }
  }
  std::vector<std::string> named;
  std::size_t              call = 0;
  for(auto const& entry : file.records)
  {
    if(entry.record.kind == trace_kind::mmio)
    {
      REQUIRE(entry.name.has_value());
      named.push_back(*entry.name);
    }
    if(entry.record.kind == trace_kind::call)
    {
      ++call;
      CHECK(entry.name == "cpu_accesses");
    }
  }
  CHECK(named.size() == 10);
  CHECK(named == std::vector<std::string>(expected.end() - 10, expected.end()));
  CHECK(call == 1);
}
TEST_CASE("a restore between slices writes a time record and times continue from it")
{
  fixture f{m33(), "alu", {}};
  f.set("r0", 1000);
  auto const taken  = f.target->snapshot();
  auto const path   = scratch("alu.pctr");
  auto       writer = obs::pctr_writer_for(*f.target, path);
  auto       sink   = f.target->add_trace_sink({trace_kind::block}, writer->sink());
  static_cast<void>(f.run({.wall_budget = std::chrono::seconds{10}, .budget_left = icount{500}}));
  auto const before = f.target->virtual_clock().now();
  f.target->restore(taken);
  auto const restored = f.target->virtual_clock().now();
  static_cast<void>(f.run({.wall_budget = std::chrono::seconds{10}, .budget_left = icount{500}}));
  sink.reset();
  writer->close();

  auto const file = decode(path);
  auto const time_at =
      std::ranges::find_if(file.records | std::views::drop(1), [](decoded_record const& entry) {
        return entry.record.kind == trace_kind::time;
      });
  REQUIRE(time_at != (file.records | std::views::drop(1)).end());
  CHECK((*time_at).time == restored.value);
  CHECK(before.value > restored.value);
  auto const after = std::ranges::next(time_at);
  REQUIRE(after != (file.records | std::views::drop(1)).end());
  CHECK((*after).time > restored.value);
}

TEST_CASE("ids that overflowed resolve by address, unknown ones stay overflowed")
{
  auto const       path = scratch("overflow.pctr");
  obs::pctr_writer writer{
      path,
      icount{100},
      {.registers       = {},
             .register_ranges = {{.start = 0x4000'0000, .size = 4, .name = "UART.DR"}},
             .symbols         = {"main"},
             .symbol_ranges   = {{.start = 0x0800'0100, .size = 0x20, .name = "handler"}}}
  };
  std::array const records{
      trace_record{
                   .kind    = trace_kind::mmio,
                   .flags   = 4,
                   .aux     = core::trace_id_overflow,
                   .dt      = 1,
                   .payload = {0x0800'0000, 0x4000'0000, 7}},
      trace_record{
                   .kind    = trace_kind::call,
                   .aux     = core::trace_id_overflow,
                   .dt      = 2,
                   .payload = {0x0800'0110, 3, 0}},
      trace_record{
                   .kind    = trace_kind::mmio,
                   .flags   = 4,
                   .aux     = core::trace_id_overflow,
                   .dt      = 3,
                   .payload = {0, 0x5000'0000, 0}},
      trace_record{.kind = trace_kind::call, .aux = 0, .dt = 4, .payload = {0x0800'0200, 0, 0}},
      trace_record{
                   .kind = trace_kind::mmio, .flags = 4, .aux = 7, .dt = 5, .payload = {0, 0x4000'0000, 0}},
  };
  writer.write(records);
  writer.close();
  auto const file = decode(path);
  REQUIRE(file.records.size() == 6);
  CHECK(file.records[0].time == 100);
  CHECK(file.records[1].name == "UART.DR");
  CHECK(file.records[2].name == "handler");
  CHECK(file.records[3].record.aux == core::trace_id_overflow);
  CHECK(file.records[4].name == "main");
  CHECK(file.records[5].name == "UART.DR");
  CHECK(file.records[5].time == 115);
  CHECK(file.strings == std::vector<std::string>{"UART.DR", "handler", "main"});
}

TEST_CASE("a write that cannot reach the disk is reported by close")
{
  obs::pctr_writer writer{"/dev/full", icount{0}, {}};
  std::array const records{
      trace_record{.kind = trace_kind::block, .aux = 3, .dt = 3, .payload = {0x100, 0, 0}}
  };
  writer.write(records);
  CHECK_THROWS_AS(writer.close(), obs::format_error);
  CHECK_NOTHROW(writer.close());
  CHECK_THROWS_AS(writer.write(records), obs::format_error);
}

TEST_CASE("a file that cannot be created is refused")
{
  CHECK_THROWS_AS(
      obs::pctr_writer("/nonexistent-directory/x.pctr", icount{0}, {}), obs::format_error);
}

TEST_CASE("string ids stop at 0xfffe, later names stay overflowed without a string record")
{
  auto const      path = scratch("full.pctr");
  obs::pctr_names names;
  for(std::uint32_t id = 0; id < 0xffff; ++id)
  {
    names.registers.push_back(std::format("P.R{}", id));
  }
  names.register_ranges = {
      {.start = 0x4000'0000, .size = 4, .name = "LATE.A"},
      {.start = 0x4000'0004, .size = 4, .name = "LATE.B"}
  };
  obs::pctr_writer          writer{path, icount{0}, std::move(names)};
  std::vector<trace_record> records;
  for(std::uint32_t id = 0; id < 0xffff; ++id)
  {
    records.push_back(
        {.kind = trace_kind::mmio, .flags = 4, .aux = static_cast<std::uint16_t>(id), .dt = 1});
  }
  records.push_back({
      .kind    = trace_kind::mmio,
      .flags   = 4,
      .aux     = core::trace_id_overflow,
      .dt      = 1,
      .payload = {0, 0x4000'0000, 0}
  });
  records.push_back({
      .kind    = trace_kind::mmio,
      .flags   = 4,
      .aux     = core::trace_id_overflow,
      .dt      = 1,
      .payload = {0, 0x4000'0004, 0}
  });
  REQUIRE(records.size() == 65537);
  writer.write(records);
  writer.close();
  auto const file = decode(path);
  REQUIRE(file.records.size() == 65538);
  CHECK(file.strings.size() == 0xffff);
  CHECK(file.records[1].record.aux == 0);
  CHECK(file.records[0xffff].record.aux == 0xfffe);
  CHECK(file.records[0xffff].name == "P.R65534");
  CHECK(file.records[0x10000].record.aux == core::trace_id_overflow);
  CHECK(file.records[0x10001].record.aux == core::trace_id_overflow);
  CHECK(std::ranges::find(file.strings, std::string{"LATE.A"}) == file.strings.end());
}

TEST_CASE("a name over 65535 bytes is cut")
{
  auto const       path = scratch("long.pctr");
  obs::pctr_writer writer{path, icount{0}, {.registers = {std::string(70'000, 'n')}}};
  std::array const records{
      trace_record{.kind = trace_kind::mmio, .flags = 4, .aux = 0, .dt = 1}
  };
  writer.write(records);
  writer.close();
  auto const file = decode(path);
  REQUIRE(file.strings.size() == 1);
  CHECK(file.strings[0] == std::string(65'535, 'n'));
  CHECK(file.records.back().name == file.strings[0]);
}

TEST_CASE("an id past the name table falls back to the address")
{
  auto const       path = scratch("past.pctr");
  obs::pctr_writer writer{
      path,
      icount{0},
      {.registers       = {"P.A"},
             .register_ranges = {{.start = 0x4000'0000, .size = 4, .name = "P.B"}}}
  };
  std::array const records{
      trace_record{
                   .kind = trace_kind::mmio, .flags = 4, .aux = 9, .dt = 1, .payload = {0, 0x4000'0000, 0}},
      trace_record{
                   .kind = trace_kind::mmio, .flags = 4, .aux = 9, .dt = 1, .payload = {0, 0x5000'0000, 0}},
  };
  writer.write(records);
  writer.close();
  auto const file = decode(path);
  CHECK(file.records[1].name == "P.B");
  CHECK(file.records[2].record.aux == core::trace_id_overflow);
}
