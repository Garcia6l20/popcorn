#include <popcorn/arch/armv8m/armv8m.hpp>
#include <popcorn/core/description.hpp>
#include <popcorn/core/errors.hpp>
#include <popcorn/core/log.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/core/snapshot.hpp>

#include "../../../native/src/core/machine_impl.hpp"
#include "../support/machine_fixture.hpp"
#include "../support/test_arch.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <ranges>
#include <string_view>
#include <vector>

namespace
{
namespace core = popcorn::core;
using popcorn::address;
using popcorn::stop_reason;
using popcorn::testing::sym;
using fixture = popcorn::testing::machine_fixture<>;

constexpr address pair_cr    = 0x4003'0000;
constexpr address second_dr  = 0x4003'0100;
constexpr address gap        = 0x4003'0200;
constexpr address hole       = 0x4003'0300;
constexpr address common_ccr = 0x4003'0308;
constexpr address sec_ccr    = 0x5003'0308;
constexpr address sec_dr     = 0x5003'0100;
constexpr address wide_cr    = 0x4004'0000;
constexpr address wide_tail  = 0x4004'04fc;
constexpr address last_dr    = 0x4004'0600;

auto shared() -> popcorn::part_description
{
  return popcorn::testing::description_from("POPCORN_M33_SHARED_DESC");
}

auto booted(bool strict = false) -> fixture
{
  core::machine_options options;
  options.strict_peripherals = strict;
  fixture f{shared(), "periph", std::move(options)};
  REQUIRE(f.run_to_stop().reason == stop_reason::user);
  return f;
}

auto word(fixture const& f, address at) -> std::uint32_t
{
  auto const value = popcorn::core::machine_internals::services(*f.target).read(
      at, 4, popcorn::initiator::debugger);
  REQUIRE(value.has_value());
  return *value;
}

auto results(fixture const& f) -> std::array<std::uint32_t, 8>
{
  std::array<std::uint32_t, 8> out{};
  for(auto const [index, value] : std::views::enumerate(out))
  {
    value = word(f, sym("periph", "results") + 4 * static_cast<address>(index));
  }
  return out;
}

auto warnings(std::vector<core::log_record> const& records) -> std::vector<core::log_record>
{
  return records
       | std::views::filter([](core::log_record const& record) {
           return record.kind == core::log_kind::model_warning;
         })
       | std::ranges::to<std::vector>();
}

auto refused(auto&& edit) -> bool
{
  auto desc = shared();
  edit(desc);
  try
  {
    fixture const f{std::move(desc), "periph", {}};
  }
  catch(popcorn::config_error const&)
  {
    return true;
  }
  return false;
}

auto unit(popcorn::part_description& desc, std::string_view name) -> popcorn::peripheral_desc&
{
  auto const found = std::ranges::find(desc.peripherals, name, &popcorn::peripheral_desc::name);
  REQUIRE(found != desc.peripherals.end());
  return *found;
}
}

TEST_CASE("the CPU reaches each peripheral of a shared page at its own registers")
{
  auto                          f = booted();
  std::vector<core::log_record> records;
  auto const                    sink =
      f.target->add_log_sink([&](core::log_record const& r) { records.push_back(r); });
  f.set("pc", sym("periph", "shared_accesses"));
  REQUIRE(f.run_to_stop().reason == stop_reason::user);
  CHECK(results(f) == std::array<std::uint32_t, 8>{0xa1, 0xb2, 0xc3, 0xc3, 0, 0xd4, 0xe5, 0x55});
  auto const warned = warnings(records);
  REQUIRE(warned.size() == 1);
  CHECK(warned[0].peripheral == "SECOND");
  CHECK(warned[0].offset == gap - second_dr);
}

TEST_CASE("the debugger reaches the same storage as the CPU in a shared page")
{
  auto f = booted();
  CHECK(word(f, pair_cr) == 0x11);
  CHECK(word(f, second_dr) == 0x33);
  CHECK(word(f, common_ccr) == 0x44);
  CHECK(word(f, wide_tail) == 0x66);
  CHECK(word(f, last_dr) == 0x77);
  auto& bus = popcorn::core::machine_internals::services(*f.target);
  REQUIRE(bus.write(sec_ccr, 4, 0x1234, popcorn::initiator::debugger));
  REQUIRE(bus.write(sec_dr, 4, 0x5678, popcorn::initiator::debugger));
  CHECK(word(f, common_ccr) == 0x1234);
  CHECK(word(f, second_dr) == 0x5678);
  CHECK(word(f, pair_cr) == 0x11);
  CHECK(word(f, gap) == 0);
}

TEST_CASE("watches see CPU accesses to a shared page, at a gap address as at a register")
{
  auto                         f = booted();
  std::vector<core::watch_hit> hits;
  auto const                   record = [&hits](core::watch_hit const& hit) {
    hits.push_back(hit);
    return core::watch_action::resume;
  };
  auto const ccr   = f.target->add_watch(common_ccr, 4, core::watch_kind::write, record);
  auto const empty = f.target->add_watch(gap, 4, core::watch_kind::read, record);
  f.set("pc", sym("periph", "shared_accesses"));
  REQUIRE(f.run_to_stop().reason == stop_reason::user);
  REQUIRE(hits.size() == 2);
  CHECK(hits[0].addr == common_ccr);
  CHECK(hits[0].write);
  CHECK(hits[0].value == 0xc3);
  CHECK(hits[1].addr == gap);
  CHECK_FALSE(hits[1].write);
  CHECK(hits[1].value == 0);
  CHECK(hits[1].by == popcorn::initiator::cpu);
}

TEST_CASE("a CPU write past a peripheral's registers or in a hole between them fires a write watch")
{
  auto                         f = booted();
  std::vector<core::watch_hit> hits;
  auto const                   record = [&hits](core::watch_hit const& hit) {
    hits.push_back(hit);
    return core::watch_action::resume;
  };
  auto const past_write = f.target->add_watch(gap, 4, core::watch_kind::write, record);
  auto const past_read  = f.target->add_watch(gap, 4, core::watch_kind::read, record);
  auto const hole_write = f.target->add_watch(hole, 4, core::watch_kind::write, record);
  auto const hole_read  = f.target->add_watch(hole, 4, core::watch_kind::read, record);
  f.set("pc", sym("periph", "gap_writes"));
  REQUIRE(f.run_to_stop().reason == stop_reason::user);
  REQUIRE(hits.size() == 2);
  CHECK(hits[0].addr == gap);
  CHECK(hits[0].write);
  CHECK(hits[0].size == 2);
  CHECK(hits[0].value == 0xa5a5);
  CHECK(hits[1].addr == hole + 1);
  CHECK(hits[1].write);
  CHECK(hits[1].size == 1);
  CHECK(hits[1].value == 0xa5);
}

TEST_CASE("a CPU read in a hole between registers fires a read watch with 0")
{
  auto                         f = booted();
  std::vector<core::watch_hit> hits;
  auto const                   record = [&hits](core::watch_hit const& hit) {
    hits.push_back(hit);
    return core::watch_action::resume;
  };
  auto const hole_read  = f.target->add_watch(hole, 4, core::watch_kind::read, record);
  auto const hole_write = f.target->add_watch(hole, 4, core::watch_kind::write, record);
  REQUIRE(f.target->mem().write(sym("periph", "results"), 4, 0xffff'ffff));
  f.set("pc", sym("periph", "hole_read"));
  REQUIRE(f.run_to_stop().reason == stop_reason::user);
  CHECK(results(f)[0] == 0);
  REQUIRE(hits.size() == 1);
  CHECK(hits[0].addr == hole);
  CHECK_FALSE(hits[0].write);
  CHECK(hits[0].value == 0);
}

TEST_CASE("a DMA access to a gap or a hole fires a watch with the dma initiator")
{
  auto                         f = booted();
  std::vector<core::watch_hit> hits;
  auto const                   record = [&hits](core::watch_hit const& hit) {
    hits.push_back(hit);
    return core::watch_action::resume;
  };
  auto const all_gap  = f.target->add_watch(gap, 4, core::watch_kind::access, record);
  auto const all_hole = f.target->add_watch(hole, 4, core::watch_kind::access, record);
  auto&      services = f.module->attached_services();
  REQUIRE(services.write(gap, 4, 0x1234, popcorn::initiator::dma));
  REQUIRE(services.read(gap, 4, popcorn::initiator::dma));
  REQUIRE(services.write(hole, 4, 0x5678, popcorn::initiator::dma));
  REQUIRE(services.read(hole, 4, popcorn::initiator::dma));
  REQUIRE(hits.size() == 4);
  CHECK(
      std::ranges::all_of(hits, [](auto const& hit) { return hit.by == popcorn::initiator::dma; }));
  CHECK(hits[0].addr == gap);
  CHECK(hits[0].write);
  CHECK(hits[0].value == 0x1234);
  CHECK(hits[1].addr == gap);
  CHECK_FALSE(hits[1].write);
  CHECK(hits[1].value == 0);
  CHECK(hits[2].addr == hole);
  CHECK(hits[2].write);
  CHECK(hits[2].value == 0x5678);
  CHECK(hits[3].addr == hole);
  CHECK_FALSE(hits[3].write);
  CHECK(hits[3].value == 0);
}

TEST_CASE("a debugger access to a gap or a hole fires no watch")
{
  auto                         f = booted();
  std::vector<core::watch_hit> hits;
  auto const                   record = [&hits](core::watch_hit const& hit) {
    hits.push_back(hit);
    return core::watch_action::resume;
  };
  auto const all_gap  = f.target->add_watch(gap, 4, core::watch_kind::access, record);
  auto const all_hole = f.target->add_watch(hole, 4, core::watch_kind::access, record);
  CHECK(word(f, gap) == 0);
  CHECK(word(f, hole) == 0);
  CHECK(hits.empty());
}

TEST_CASE("a gap read in a shared page under strict_peripherals warns and reads 0")
{
  auto                          f = booted(true);
  std::vector<core::log_record> records;
  auto const                    sink =
      f.target->add_log_sink([&](core::log_record const& r) { records.push_back(r); });
  REQUIRE(f.target->mem().write(sym("periph", "results"), 4, 0xffff'ffff));
  f.set("pc", sym("periph", "gap_read"));
  REQUIRE(f.run_to_stop().reason == stop_reason::user);
  CHECK(results(f)[0] == 0);
  REQUIRE(records.size() == 1);
  CHECK(records[0].kind == core::log_kind::model_warning);
  CHECK(records[0].peripheral == "SECOND");
}

TEST_CASE("a snapshot round trip restores every peripheral of a shared page")
{
  auto f = booted();
  f.set("pc", sym("periph", "shared_accesses"));
  REQUIRE(f.run_to_stop().reason == stop_reason::user);
  auto const saved = f.target->snapshot();
  for(auto const at : {pair_cr, second_dr, common_ccr, wide_tail, last_dr})
  {
    REQUIRE(
        popcorn::core::machine_internals::services(*f.target).write(
            at, 4, 0xdead, popcorn::initiator::debugger));
  }
  f.target->restore(saved);
  CHECK(word(f, pair_cr) == 0xa1);
  CHECK(word(f, second_dr) == 0xb2);
  CHECK(word(f, common_ccr) == 0xc3);
  CHECK(word(f, wide_tail) == 0xe5);
  CHECK(word(f, last_dr) == 0xd4);
  CHECK(word(f, wide_cr) == 0x55);
}

TEST_CASE("peripherals that overlap or cannot share a page are refused")
{
  CHECK_FALSE(refused([](popcorn::part_description&) {}));
  CHECK(refused([](popcorn::part_description& desc) { unit(desc, "PAIR").size = 0x104; }));
  CHECK(refused([](popcorn::part_description& desc) { unit(desc, "PAIR").base = 0x4003'0020; }));
  CHECK(refused([](popcorn::part_description& desc) { unit(desc, "SECOND").base = 0x4003'0102; }));
  CHECK(refused(
      [](popcorn::part_description& desc) { unit(desc, "UNMODELLED").base = 0x4002'0100; }));
}

TEST_CASE(
    "an STM32U575 machine reaches ADC12 in ADC1's page, and through SEC_ADC12"
    * doctest::skip(std::getenv("POPCORN_U575_DESC") == nullptr))
{
  auto const machine = std::make_unique<core::machine>(
      popcorn::testing::description_from("POPCORN_U575_DESC"), popcorn::arch::armv8m::factory(),
      core::machine_options{});
  auto const& units   = machine->description().peripherals;
  auto const  base_of = [&units](std::string_view name) {
    auto const found = std::ranges::find(units, name, &popcorn::peripheral_desc::name);
    REQUIRE(found != units.end());
    return found->base;
  };
  auto const ccr        = base_of("ADC12") + 8;
  auto const secure_ccr = base_of("SEC_ADC12") + 8;
  REQUIRE(base_of("ADC1") / 1024 == ccr / 1024);
  auto& bus = popcorn::core::machine_internals::services(*machine);
  REQUIRE(bus.write(ccr, 4, 0x0001'0000, popcorn::initiator::debugger));
  CHECK(bus.read(ccr, 4, popcorn::initiator::debugger) == 0x0001'0000u);
  CHECK(bus.read(secure_ccr, 4, popcorn::initiator::debugger) == 0x0001'0000u);
  REQUIRE(bus.write(secure_ccr, 4, 0x0002'0000, popcorn::initiator::debugger));
  CHECK(bus.read(ccr, 4, popcorn::initiator::debugger) == 0x0002'0000u);
}
