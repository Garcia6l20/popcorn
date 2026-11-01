#include "stm32_fixture.hpp"

#include <popcorn/core/errors.hpp>
#include <popcorn/core/log.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/core/services.hpp>
#include <popcorn/periph/model.hpp>
#include <popcorn/periph/stm32/board.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
namespace core   = popcorn::core;
namespace periph = popcorn::periph;
using popcorn::address;
using popcorn::icount;
using popcorn::initiator;
using popcorn::part_description;
using popcorn::stop_reason;
using popcorn::testing::stm32_fixture;
using popcorn::testing::sym;

class bus_probe final : public periph::model
{
public:
  explicit bus_probe(periph::model_context& ctx) : context{ctx}
  {}

  periph::model_context& context;

  [[nodiscard]] auto claims() const -> std::vector<std::string> override
  {
    return {};
  }
  void reset(periph::register_file&, popcorn::reset_kind) override
  {}
  auto on_read(periph::register_file&, std::uint32_t, std::uint8_t, periph::access_ctx)
      -> std::expected<std::uint32_t, popcorn::bus_error> override
  {
    return 0u;
  }
  auto on_write(
      periph::register_file&,
      std::uint32_t,
      std::uint8_t,
      std::uint32_t,
      std::uint32_t,
      periph::access_ctx) -> std::expected<void, popcorn::bus_error> override
  {
    return {};
  }
  void on_event(std::uint32_t, std::uint64_t) override
  {}
  void save(std::vector<std::byte>&) const override
  {}
  void load(std::span<std::byte const>) override
  {}
};

struct part
{
  part_description const& (*desc)();
  std::string_view firmware;
  std::string_view probe_host;
  std::string_view peripheral;
  std::string_view short_window;
};

struct rig
{
  std::unique_ptr<stm32_fixture> f;
  std::vector<core::log_record>  logged;
  core::hook_handle              sink;
  std::string                    probe_host;

  rig(part const& on, bool strict) : probe_host{on.probe_host}
  {
    auto options               = popcorn::testing::stm32_options();
    options.strict_peripherals = strict;
    options.models             = periph::stm32::factories(periph::stm32::board{}, on.desc());
    options.models.insert_or_assign(
        std::string{on.probe_host},
        [](periph::model_context& ctx, popcorn::peripheral_desc const&) {
          return std::make_shared<bus_probe>(ctx);
        });
    f    = std::make_unique<stm32_fixture>(on.desc(), on.firmware, std::move(options));
    sink = f->target->add_log_sink(
        [this](core::log_record const& record) { logged.push_back(record); });
    f->set("pc", sym(on.firmware, "idle"));
  }

  [[nodiscard]] auto probe() const -> bus_probe&
  {
    auto* const found = f->model_of<bus_probe>(probe_host);
    REQUIRE(found != nullptr);
    return *found;
  }

  [[nodiscard]] auto read(address at, initiator by) const
  {
    return probe().context.services.read(at, 4, by);
  }

  [[nodiscard]] auto write(address at, std::uint32_t value, initiator by) const
  {
    return probe().context.services.write(at, 4, value, by);
  }

  [[nodiscard]] auto warnings(std::string_view text) const -> std::ptrdiff_t
  {
    return std::ranges::count_if(logged, [&](core::log_record const& record) {
      return record.kind == core::log_kind::model_warning
         and record.message.find(text) != std::string::npos;
    });
  }
};

auto l152re_part() -> part
{
  return {&popcorn::testing::l152re, "l152xe_l1_iwdg_ll", "DAC", "TIM2", "DBGMCU"};
}

auto u575_part() -> part
{
  return {&popcorn::testing::u575, "rcc_ll", "DAC1", "CRC", "ADC1"};
}

auto h573_part() -> part
{
  return {&popcorn::testing::h573, "h573_rcc_ll", "DAC", "CRC", "ADC1"};
}

struct holes
{
  address inside;
  address beyond;
};

auto unit_of(part const& on, std::string_view name) -> popcorn::peripheral_desc const&
{
  auto const& desc  = on.desc();
  auto const  found = std::ranges::find(desc.peripherals, name, &popcorn::peripheral_desc::name);
  REQUIRE(found != desc.peripherals.end());
  return *found;
}

auto holes_of(part const& on) -> holes
{
  auto const& unit    = unit_of(on, on.peripheral);
  auto const  covered = [&](std::uint32_t offset) {
    return std::ranges::any_of(unit.registers, [&](popcorn::register_desc const& reg) {
      return offset >= reg.offset and offset < reg.offset + reg.width / 8u;
    });
  };
  std::uint32_t last = 0;
  for(auto const& reg : unit.registers)
  {
    last = std::max(last, reg.offset + reg.width / 8u);
  }
  std::uint32_t inside = 0;
  while(inside < last and covered(inside))
  {
    inside += 4;
  }
  REQUIRE(inside < last);
  auto const& outside = unit_of(on, on.short_window);
  REQUIRE(outside.size % 1024 != 0);
  return {.inside = unit.base + inside, .beyond = outside.base + ((outside.size + 3u) & ~3u)};
}

auto parts() -> std::vector<part>
{
  return {l152re_part(), u575_part(), h573_part()};
}
}

TEST_CASE(
    "under strict peripherals a CPU or DMA read or write of an offset no register covers raises "
    "model_error, in a hole of the register map and past the description's size, on L1, U5 and H5")
{
  for(auto const& on : parts())
  {
    CAPTURE(on.firmware);
    auto const at = holes_of(on);
    for(auto const where : {at.inside, at.beyond})
    {
      CAPTURE(where);
      rig const r{on, true};
      CHECK_THROWS_AS(static_cast<void>(r.read(where, initiator::cpu)), popcorn::model_error);
      CHECK_THROWS_AS(
          static_cast<void>(r.write(where, 0x5A, initiator::cpu)), popcorn::model_error);
      CHECK_THROWS_AS(static_cast<void>(r.read(where, initiator::dma)), popcorn::model_error);
      CHECK_THROWS_AS(
          static_cast<void>(r.write(where, 0x5A, initiator::dma)), popcorn::model_error);
      CHECK(r.warnings("no register there") == 1);
    }
  }
}

TEST_CASE(
    "under strict peripherals a debugger access to an offset no register covers reads 0, "
    "drops the write and logs nothing")
{
  for(auto const& on : parts())
  {
    CAPTURE(on.firmware);
    auto const at = holes_of(on);
    for(auto const where : {at.inside, at.beyond})
    {
      CAPTURE(where);
      rig const r{on, true};
      REQUIRE(r.write(where, 0x5A, initiator::debugger).has_value());
      auto const read = r.read(where, initiator::debugger);
      REQUIRE(read.has_value());
      CHECK(*read == 0);
      CHECK(r.logged.empty());
    }
  }
}

TEST_CASE(
    "without strict peripherals an offset no register covers reads 0 and logs once per "
    "offset, CPU, DMA and debugger alike")
{
  for(auto const& on : parts())
  {
    CAPTURE(on.firmware);
    auto const at = holes_of(on);
    for(auto const by : {initiator::cpu, initiator::dma, initiator::debugger})
    {
      CAPTURE(static_cast<int>(by));
      rig const r{on, false};
      for(auto const where : {at.inside, at.beyond})
      {
        auto const read = r.read(where, by);
        REQUIRE(read.has_value());
        CHECK(*read == 0);
        REQUIRE(r.write(where, 0x5A, by).has_value());
      }
      CHECK(r.warnings("no register there") == 2);
    }
  }
}

TEST_CASE("a firmware load from an L1 hole of the register map fails a strict run with model_error")
{
  auto const on = l152re_part();
  auto const at = holes_of(on);
  rig const  r{on, true};
  auto const code = address{0x2000'1000};
  REQUIRE(r.f->target->mem().write(code, 2, 0x6808).has_value());
  REQUIRE(r.f->target->mem().write(code + 2, 2, 0xBE00).has_value());
  r.f->set("r1", at.inside);
  r.f->set("pc", code);
  CHECK_THROWS_AS(
      static_cast<void>(r.f->run_to_stop({.max_instructions = icount{100}})), popcorn::model_error);
}

TEST_CASE(
    "under strict peripherals a CPU write to a hole of an L1 TIM2 held in RCC reset raises "
    "model_error, as past the description's size")
{
  auto const on = l152re_part();
  auto const at = holes_of(on);
  rig const  r{on, true};
  popcorn::testing::rcc_reset_write(*r.f, "APB1RSTR", "TIM2RST", 1);
  REQUIRE(r.f->target->held_in_reset("TIM2"));
  CHECK_THROWS_AS(
      static_cast<void>(r.write(at.inside, 0x5A, initiator::cpu)), popcorn::model_error);
  CHECK(r.warnings("no register there") == 1);
}
