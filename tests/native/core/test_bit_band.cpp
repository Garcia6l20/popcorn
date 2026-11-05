#include <popcorn/core/errors.hpp>
#include <popcorn/core/log.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/core/services.hpp>

#include "../../../native/src/core/memory_map.hpp"
#include "../support/machine_fixture.hpp"
#include "../support/test_model.hpp"

#include <doctest/doctest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <ranges>
#include <string>
#include <vector>

namespace
{
namespace core = popcorn::core;
using popcorn::address;
using popcorn::bus_attrs;
using popcorn::bus_error;
using popcorn::initiator;
using popcorn::region_desc;
using popcorn::region_kind;

constexpr address       sram_alias   = 0x2200'0000;
constexpr address       sram         = 0x2000'0000;
constexpr address       periph_alias = 0x4200'0000;
constexpr address       periph       = 0x4000'0000;
constexpr address       testdev      = 0x4001'0000;
constexpr address       rw_register  = testdev + 0x08;
constexpr address       w1c_register = testdev + 0x18;
constexpr address       r_clear      = testdev + 0x3c;
constexpr address       code         = 0x200a'0000;
constexpr address       target_word  = 0x2000'0100;
constexpr std::uint32_t band_size    = 0x0200'0000;

constexpr std::uint16_t str_r2_r1  = 0x600a;
constexpr std::uint16_t strh_r2_r1 = 0x800a;
constexpr std::uint16_t strb_r2_r1 = 0x700a;
constexpr std::uint16_t ldr_r0_r1  = 0x6808;
constexpr std::uint16_t ldrh_r0_r1 = 0x8808;
constexpr std::uint16_t ldrb_r0_r1 = 0x7808;
constexpr std::uint16_t bkpt       = 0xbe00;
constexpr std::uint16_t nop        = 0xbf00;
constexpr std::uint16_t movs_r0_1  = 0x2001;
constexpr std::uint16_t bx_r3      = 0x4718;

constexpr auto alias_of(address alias_base, address target_base, address byte, unsigned bit)
    -> address
{
  return alias_base + (byte - target_base) * 32 + bit * 4;
}

auto band(std::string name, address start, address physical) -> region_desc
{
  return {
      .name       = std::move(name),
      .start      = start,
      .size       = band_size,
      .kind       = region_kind::bit_band,
      .acc        = {.read = true, .write = true, .execute = false},
      .secure     = false,
      .physical   = physical,
      .controller = std::nullopt,
      .content    = std::nullopt,
      .fill       = std::nullopt,
  };
}

auto banded() -> popcorn::part_description
{
  auto desc     = popcorn::testing::m33();
  desc.core.cpu = "CM3";
  desc.regions.push_back(band("BB_SRAM", sram_alias, sram));
  desc.regions.push_back(band("BB_PERIPH", periph_alias, periph));
  return desc;
}

struct rig
{
  popcorn::testing::test_arch*   module = nullptr;
  std::unique_ptr<core::machine> target;

  explicit rig(popcorn::part_description desc = banded(), core::machine_options options = {})
  {
    target = std::make_unique<core::machine>(
        std::move(desc),
        [this](popcorn::core_desc const& unit) -> std::unique_ptr<popcorn::arch::arch> {
          auto made = std::make_unique<popcorn::testing::test_arch>(unit);
          module    = made.get();
          return made;
        },
        std::move(options));
    target->reset();
  }

  void set(std::string_view name, std::uint64_t value) const
  {
    target->regs().write(*target->regs().find(name), value);
  }

  [[nodiscard]] auto get(std::string_view name) const -> std::uint64_t
  {
    return target->regs().read(*target->regs().find(name));
  }

  [[nodiscard]] auto run_from(address at) const -> popcorn::stop
  {
    set("pc", at);
    for(int slice = 0; slice < 16; ++slice)
    {
      auto const result = target->run_slice(
          {.max_instructions = popcorn::icount{1000}, .wall_budget = std::chrono::seconds{10}});
      if(result.stopped)
      {
        return *result.stopped;
      }
    }
    FAIL("no stop after 16 slices");
    return {};
  }

  auto cpu(std::uint16_t insn, address at, std::uint32_t value = 0, std::size_t nops = 0) const
      -> std::uint32_t
  {
    std::vector<std::uint16_t> words(nops, nop);
    words.push_back(insn);
    words.push_back(bkpt);
    REQUIRE(target->mem().write_block(code, std::as_bytes(std::span{words})));
    set("r1", at);
    set("r2", value);
    set("r0", 0xdead'beef);
    auto const stopped = run_from(code);
    REQUIRE(stopped.reason == popcorn::stop_reason::user);
    return static_cast<std::uint32_t>(get("r0"));
  }

  [[nodiscard]] auto word(address at) const -> std::uint32_t
  {
    auto const value = target->mem().read(at, 4);
    REQUIRE(value.has_value());
    return *value;
  }

  void poke(address at, std::uint32_t value) const
  {
    REQUIRE(target->mem().write(at, 4, value));
  }

  [[nodiscard]] auto peek(address at) const -> std::uint32_t
  {
    return target->peripheral("TESTDEV").peek(at - testdev);
  }

  [[nodiscard]] auto services() const -> core::machine_services&
  {
    return module->attached_services();
  }
};

auto mmio_records(core::machine& target) -> std::vector<core::trace_record>
{
  return target.trace().records()
       | std::views::filter([](auto const& r) { return r.kind == core::trace_kind::mmio; })
       | std::ranges::to<std::vector>();
}
}

TEST_CASE("DDI0337H 3.7: alias word w maps bit w mod 8 of target byte w / 8")
{
  auto const sram_band = band("BB_SRAM", sram_alias, sram);
  CHECK(core::bit_band_target(sram_band, 0x2200'0000, 4).unit == 0x2000'0000);
  CHECK(core::bit_band_target(sram_band, 0x2200'0000, 4).bit == 0);
  CHECK(core::bit_band_target(sram_band, 0x2200'001c, 4).unit == 0x2000'0000);
  CHECK(core::bit_band_target(sram_band, 0x2200'001c, 4).bit == 7);
  CHECK(core::bit_band_target(sram_band, 0x23ff'ffe0, 4).unit == 0x200f'fffc);
  CHECK(core::bit_band_target(sram_band, 0x23ff'ffe0, 4).bit == 24);
  CHECK(core::bit_band_target(sram_band, 0x23ff'fffc, 4).bit == 31);
  CHECK(core::bit_band_target(sram_band, 0x23ff'ffe0, 1).unit == 0x200f'ffff);
  CHECK(core::bit_band_target(sram_band, 0x23ff'ffe0, 1).bit == 0);
  CHECK(core::bit_band_target(sram_band, 0x23ff'fffc, 2).unit == 0x200f'fffe);
  CHECK(core::bit_band_target(sram_band, 0x23ff'fffc, 2).bit == 15);
  CHECK(core::bit_band_target(sram_band, 0x2200'0020, 4).unit == 0x2000'0000);
  CHECK(core::bit_band_target(sram_band, 0x2200'0020, 4).bit == 8);
  CHECK(core::bit_band_target(sram_band, 0x2200'0020, 1).unit == 0x2000'0001);
  CHECK(core::bit_band_target(sram_band, 0x2200'0020, 1).bit == 0);
}

TEST_CASE("a bit_band region gets no host buffer, no snapshot image and survives a power cycle")
{
  rig const r;
  CHECK_FALSE(r.target->mem().is_plain_memory(sram_alias, 4));
  CHECK_FALSE(r.target->mem().is_plain_memory(periph_alias, 4));
  r.poke(target_word, 0x0000'0100);
  CHECK(r.word(alias_of(sram_alias, sram, target_word + 1, 0)) == 1);
  auto const taken = r.target->snapshot();
  for(auto const& image : taken.data().memory)
  {
    CHECK(image.region != "BB_SRAM");
    CHECK(image.region != "BB_PERIPH");
  }
  CHECK(taken.data().memory.size() == 2);
  r.poke(target_word, 0);
  r.target->restore(taken);
  CHECK(r.word(alias_of(sram_alias, sram, target_word + 1, 0)) == 1);
  r.target->power_cycle();
  CHECK(r.word(target_word) == 0);
  CHECK(r.word(alias_of(sram_alias, sram, target_word + 1, 0)) == 0);
  REQUIRE(r.target->mem().write(alias_of(sram_alias, sram, target_word, 3), 4, 1));
  CHECK(r.word(target_word) == 0x8);
}

TEST_CASE("a bit_band region without physical is refused at build")
{
  auto desc = banded();
  desc.regions.back().physical.reset();
  CHECK_THROWS_AS(rig{std::move(desc)}, popcorn::config_error);
}

TEST_CASE("a bit_band region with content is refused at build")
{
  auto desc                   = banded();
  desc.regions.back().content = std::vector<popcorn::region_content>{
      {.offset = 0, .data = {1}}
  };
  CHECK_THROWS_AS(rig{std::move(desc)}, popcorn::config_error);
}

TEST_CASE("a bit_band window mapping onto itself is refused at build, not recursed into")
{
  auto desc = banded();
  desc.regions.push_back(band("BB_SELF", 0x4400'0000, 0x4400'0000));
  CHECK_THROWS_AS(rig{std::move(desc)}, popcorn::config_error);
}

TEST_CASE("a bit_band window ending at the top of the address space mapping onto itself is refused")
{
  auto desc = banded();
  desc.regions.push_back(band("BB_TOP", 0xFE00'0000, 0xFE00'0000));
  CHECK_THROWS_AS(rig{std::move(desc)}, popcorn::config_error);
}

TEST_CASE("two bit_band windows mapping onto each other are refused at build")
{
  auto desc = banded();
  desc.regions.push_back(band("BB_X", 0x4400'0000, 0x4600'0000));
  desc.regions.push_back(band("BB_Y", 0x4600'0000, 0x4400'0000));
  CHECK_THROWS_AS(rig{std::move(desc)}, popcorn::config_error);
}

TEST_CASE("DDI0337H 3.7.1: a CPU alias word write sets or clears one bit, bits 0, 7, 8 and 31")
{
  rig const r;
  r.poke(target_word, 0x5555'aaaa);
  auto expected = std::uint32_t{0x5555'aaaa};
  for(auto const bit : {0u, 7u, 8u, 31u})
  {
    CAPTURE(bit);
    auto const at = alias_of(sram_alias, sram, target_word + bit / 8, bit % 8);
    std::ignore   = r.cpu(str_r2_r1, at, 0xffff'fff1);
    expected |= 1u << bit;
    CHECK(r.word(target_word) == expected);
    CHECK(r.cpu(ldr_r0_r1, at) == 1);
    std::ignore = r.cpu(str_r2_r1, at, 0xffff'fffe);
    expected &= ~(1u << bit);
    CHECK(r.word(target_word) == expected);
    CHECK(r.cpu(ldr_r0_r1, at) == 0);
  }
}

TEST_CASE("a CPU alias read returns the bit in bit 0 and zero above it")
{
  rig const r;
  r.poke(target_word, 0x8000'0081);
  CHECK(r.cpu(ldr_r0_r1, alias_of(sram_alias, sram, target_word, 0)) == 1);
  CHECK(r.cpu(ldr_r0_r1, alias_of(sram_alias, sram, target_word, 1)) == 0);
  CHECK(r.cpu(ldr_r0_r1, alias_of(sram_alias, sram, target_word, 7)) == 1);
  CHECK(r.cpu(ldr_r0_r1, alias_of(sram_alias, sram, target_word + 3, 7)) == 1);
  CHECK(r.cpu(ldr_r0_r1, alias_of(sram_alias, sram, target_word + 3, 6)) == 0);
}

TEST_CASE("byte and halfword alias accesses reach the target unit of their own size")
{
  rig const  r;
  auto const at = alias_of(periph_alias, periph, rw_register + 1, 1);
  r.target->trace().clear(popcorn::icount{});
  std::ignore = r.cpu(strb_r2_r1, at, 1);
  CHECK(r.peek(rw_register) == 0x0200);
  CHECK(r.cpu(ldrb_r0_r1, at) == 1);
  std::ignore = r.cpu(strh_r2_r1, at, 0);
  CHECK(r.peek(rw_register) == 0);
  CHECK(r.cpu(ldrh_r0_r1, at) == 0);
  auto const records = mmio_records(*r.target);
  REQUIRE(records.size() == 6);
  auto const flags = [](std::uint8_t size, bool write) {
    return static_cast<std::uint8_t>(size | (write ? core::trace_write_flag : 0));
  };
  CHECK(records[0].flags == flags(1, false));
  CHECK(records[0].payload[1] == rw_register + 1);
  CHECK(records[1].flags == flags(1, true));
  CHECK(records[1].payload[1] == rw_register + 1);
  CHECK(records[1].payload[2] == 0x02);
  CHECK(records[2].flags == flags(1, false));
  CHECK(records[2].payload[2] == 0x02);
  CHECK(records[3].flags == flags(2, false));
  CHECK(records[3].payload[1] == rw_register);
  CHECK(records[4].flags == flags(2, true));
  CHECK(records[4].payload[1] == rw_register);
  CHECK(records[4].payload[2] == 0);
  CHECK(records[5].flags == flags(2, false));
}

TEST_CASE("the ring records the target read and write with the alias store's pc, no alias entry")
{
  rig const  r;
  auto const sink =
      r.target->add_trace_sink({core::trace_kind::mem}, [](std::span<core::trace_record const>) {});
  r.target->trace().clear(popcorn::icount{});
  std::ignore        = r.cpu(str_r2_r1, alias_of(periph_alias, periph, rw_register, 4), 1, 2);
  auto const records = mmio_records(*r.target);
  REQUIRE(records.size() == 2);
  for(auto const& record : records)
  {
    CHECK(record.payload[0] == code + 4);
    CHECK(record.payload[1] == rw_register);
  }
  CHECK((records[0].flags & core::trace_write_flag) == 0);
  CHECK((records[1].flags & core::trace_write_flag) != 0);
  CHECK(records[1].payload[2] == 0x10);
}

TEST_CASE(
    "silicon-inferred: an alias write to a write-1-to-clear register clears every set flag "
    "the read-modify-write writes back")
{
  rig const r;
  CHECK(r.peek(w1c_register) == 0x5a);
  std::ignore = r.cpu(str_r2_r1, alias_of(periph_alias, periph, w1c_register, 0), 0);
  CHECK(r.peek(w1c_register) == 0);
  r.target->reset(popcorn::reset_kind::power_on);
  CHECK(r.peek(w1c_register) == 0x5a);
  std::ignore = r.cpu(str_r2_r1, alias_of(periph_alias, periph, w1c_register, 3), 0);
  CHECK(r.peek(w1c_register) == 0x08);
}

TEST_CASE("an alias read runs the target register's read side effect")
{
  rig const r;
  CHECK(r.peek(r_clear) == 0xa5);
  CHECK(r.cpu(ldr_r0_r1, alias_of(periph_alias, periph, r_clear, 0)) == 1);
  CHECK(r.peek(r_clear) == 0);
}

TEST_CASE("a DMA access at an alias address is a bus error, the debugger is served")
{
  rig const  r;
  auto const at       = alias_of(sram_alias, sram, target_word, 2);
  auto&      services = r.services();
  auto const dma      = bus_attrs{.by = initiator::dma};
  auto const read     = services.read(at, 4, dma);
  REQUIRE_FALSE(read.has_value());
  CHECK(read.error() == bus_error::unmapped);
  auto const written = services.write(at, 4, 1, dma);
  REQUIRE_FALSE(written.has_value());
  CHECK(written.error() == bus_error::unmapped);
  CHECK(r.word(target_word) == 0);
  REQUIRE(r.target->mem().write(at, 4, 1));
  CHECK(r.word(target_word) == 0x4);
  CHECK(r.word(at) == 1);
  auto const cpu = services.read(at, 4, bus_attrs{.by = initiator::cpu});
  REQUIRE(cpu.has_value());
  CHECK(*cpu == 1);
}

TEST_CASE("an alias access to an unmapped target is a CPU bus fault at the alias address")
{
  rig const        r;
  std::array const words{ldr_r0_r1, bkpt};
  REQUIRE(r.target->mem().write_block(code, std::as_bytes(std::span{words})));
  r.set("r1", periph_alias);
  auto const stopped = r.run_from(code);
  CHECK(stopped.reason != popcorn::stop_reason::user);
  REQUIRE(stopped.fault.has_value());
  CHECK(stopped.fault->fault_address == periph_alias);
}

TEST_CASE("a watch on the target sees the alias read-modify-write as a read and a write")
{
  rig const                    r;
  std::vector<core::watch_hit> hits;
  auto const                   observe = [&](core::watch_hit const& hit) {
    hits.push_back(hit);
    return core::watch_action::resume;
  };

  SUBCASE("a peripheral register")
  {
    auto const watch = r.target->add_watch(rw_register, 4, core::watch_kind::access, observe);
    std::ignore      = r.cpu(str_r2_r1, alias_of(periph_alias, periph, rw_register, 4), 1, 2);
    REQUIRE(hits.size() == 2);
    CHECK_FALSE(hits[0].write);
    CHECK(hits[0].addr == rw_register);
    CHECK(hits[0].pc == code + 4);
    CHECK(hits[1].write);
    CHECK(hits[1].addr == rw_register);
    CHECK(hits[1].value == 0x10);
    CHECK(hits[1].by == initiator::cpu);
  }

  SUBCASE("an SRAM word")
  {
    auto const watch = r.target->add_watch(target_word, 4, core::watch_kind::access, observe);
    std::ignore      = r.cpu(str_r2_r1, alias_of(sram_alias, sram, target_word + 1, 2), 1, 2);
    REQUIRE(hits.size() == 2);
    CHECK_FALSE(hits[0].write);
    CHECK(hits[0].addr == target_word);
    CHECK(hits[0].pc == code + 4);
    CHECK(hits[1].write);
    CHECK(hits[1].addr == target_word);
    CHECK(hits[1].value == 0x400);
    CHECK(hits[1].by == initiator::cpu);
  }
}

TEST_CASE("coverage credits the alias accesses to the target register")
{
  rig const r;
  r.target->enable_coverage(
      {
          {.name = "code", .start = code, .size = 0x400}
  },
      true);
  std::ignore           = r.cpu(str_r2_r1, alias_of(periph_alias, periph, rw_register, 0), 1);
  auto const unmodelled = r.target->coverage().unmodelled;
  REQUIRE(unmodelled.size() == 2);
  for(auto const& access : unmodelled)
  {
    CHECK(access.peripheral == "TESTDEV");
    CHECK(access.reg == "RW");
    CHECK(access.pc == code);
  }
}

TEST_CASE("checklist 20: an alias write into translated code drops it in the same slice and later")
{
  rig const         r;
  constexpr address patched = 0x2000'0800;
  constexpr address patcher = 0x2000'0400;
  std::array const  body{movs_r0_1, bkpt};
  std::array const  patch{str_r2_r1, bx_r3};
  REQUIRE(r.target->mem().write_block(patched, std::as_bytes(std::span{body})));
  REQUIRE(r.target->mem().write_block(patcher, std::as_bytes(std::span{patch})));
  REQUIRE(r.run_from(patched).reason == popcorn::stop_reason::user);
  REQUIRE(r.get("r0") == 1);
  r.set("r1", alias_of(sram_alias, sram, patched, 1));
  r.set("r2", 1);
  r.set("r3", patched | 1);
  REQUIRE(r.run_from(patcher).reason == popcorn::stop_reason::user);
  CHECK(r.get("r0") == 3);
  REQUIRE(r.run_from(patched).reason == popcorn::stop_reason::user);
  CHECK(r.get("r0") == 3);
}

TEST_CASE("an alias store to a model that drives the core clock starts the rate epoch at the store")
{
  constexpr std::uint16_t nop       = 0xbf00;
  constexpr std::uint16_t str_r1_r2 = 0x6011;
  constexpr std::size_t   before    = 120;
  constexpr std::size_t   after     = 80;
  core::machine_options   options;
  options.models.emplace(
      "TESTDEV",
      [](popcorn::periph::model_context& ctx,
         popcorn::peripheral_desc const&) -> std::shared_ptr<popcorn::periph::model> {
        auto made              = std::make_shared<popcorn::testing::test_model>(ctx);
        made->drives_clock     = true;
        made->halve_rate_on_go = true;
        made->go_delay         = 1'000'000'000;
        return made;
      });
  rig const                  r{banded(), std::move(options)};
  std::vector<std::uint16_t> body(before, nop);
  body.push_back(str_r1_r2);
  body.insert(body.end(), after, nop);
  body.push_back(bkpt);
  REQUIRE(r.target->mem().write_block(code, std::as_bytes(std::span{body})));
  r.set("r1", 1);
  r.set(
      "r2", alias_of(periph_alias, periph, testdev + popcorn::testing::test_model::ctrl_offset, 0));
  auto const start = r.target->snapshot().data().clock.now.value;
  auto const rate  = r.target->virtual_clock().instruction_hz();
  REQUIRE(r.run_from(code).reason == popcorn::stop_reason::user);
  CHECK(r.target->virtual_clock().instruction_hz() * 2 == rate);
  CHECK(r.target->snapshot().data().clock.epoch_icount.value == start + before + 1);
}

TEST_CASE("a bit_band region round-trips through the description JSON as kind bit_band")
{
  auto const desc   = banded();
  auto const dumped = popcorn::dump_description(desc);
  CHECK(dumped.contains(R"("kind":"bit_band")"));
  auto const parsed = popcorn::parse_description(dumped, "dumped");
  CHECK(parsed.regions == desc.regions);
  CHECK(popcorn::description_digest(parsed) == popcorn::description_digest(desc));
}

TEST_CASE("a CPU alias store and load to a gated peripheral are lost, read 0 and warn as a write")
{
  core::machine_options options;
  options.models.emplace("TESTDEV", popcorn::testing::test_model_factory());
  rig                           r{banded(), std::move(options)};
  std::vector<core::log_record> logged;
  auto const        sink = r.target->add_log_sink([&logged](core::log_record const& record) {
    if(record.kind == core::log_kind::model_warning)
    {
      logged.push_back(record);
    }
  });
  constexpr address data = testdev + popcorn::testing::test_model::data_offset;
  REQUIRE(r.target->mem().write(data, 4, 0x10));
  r.services().gate_bus_clock("TESTDEV", true);
  auto const alias = alias_of(periph_alias, periph, data, 0);
  r.cpu(str_r2_r1, alias, 1);
  CHECK(r.peek(data) == 0x10);
  CHECK(r.cpu(ldr_r0_r1, alias_of(periph_alias, periph, data, 4)) == 0);
  REQUIRE(logged.size() == 1);
  CHECK(logged[0].write);
  CHECK(logged[0].reg == "DATA");
  r.services().gate_bus_clock("TESTDEV", false);
  CHECK(r.cpu(ldr_r0_r1, alias_of(periph_alias, periph, data, 4)) == 1);
  r.cpu(str_r2_r1, alias, 1);
  CHECK(r.peek(data) == 0x11);
}
