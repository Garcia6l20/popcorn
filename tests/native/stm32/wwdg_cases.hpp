#pragma once

#include "stm32_fixture.hpp"

#include <popcorn/core/errors.hpp>
#include <popcorn/core/intercept.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/core/snapshot.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace wwdg_test
{
namespace core = popcorn::core;
using popcorn::address;
using popcorn::icount;
using popcorn::part_description;
using popcorn::stop_reason;
using popcorn::vtime;
using popcorn::testing::stm32_fixture;
using popcorn::testing::sym;

using wide = unsigned __int128;

constexpr std::uint64_t ms       = 1'000'000'000;
constexpr std::uint64_t ps_per_s = 1'000'000'000'000;
constexpr std::uint32_t wdga     = 0x80;
constexpr std::uint32_t ewi      = 0x200;
constexpr std::uint32_t t_mask   = 0x7F;
constexpr std::uint32_t shift_at = 7;

struct rig
{
  std::string_view firmware;
  std::string_view enable_reg;
  std::uint64_t    pclk1_hz;
  std::uint32_t    irq;
  auto (*desc)() -> part_description const&;
};

auto the_rig() -> rig const&;

inline auto period_cycles(std::uint32_t shift) -> std::uint64_t
{
  return std::uint64_t{4096} << shift;
}

inline auto cycles_in(std::uint64_t elapsed_ps, std::uint64_t hz) -> std::uint64_t
{
  return static_cast<std::uint64_t>(wide{elapsed_ps} * hz / ps_per_s);
}

inline auto ps_of(std::uint64_t cycles, std::uint64_t hz) -> std::uint64_t
{
  return static_cast<std::uint64_t>((wide{cycles} * ps_per_s + hz - 1) / hz);
}

struct bench
{
  rig const&    r;
  stm32_fixture f;
  std::uint64_t t0 = 0;

  explicit bench(
      rig const&            which,
      core::machine_options options = popcorn::testing::stm32_options())
      : r{which}, f{which.desc(), which.firmware, std::move(options)}
  {
    f.set("pc", sym(r.firmware, "idle"));
    enable_clock();
  }

  void enable_clock()
  {
    popcorn::testing::enable_clocks(
        *f.target, {
                       {r.enable_reg, "WWDGEN"}
    });
    t0 = f.now().ps;
  }

  [[nodiscard]] auto at(std::string_view peripheral, std::string_view reg) const -> address
  {
    auto const& desc  = f.target->peripheral(peripheral).description();
    auto const  found = popcorn::find_register(desc, reg);
    REQUIRE(found != nullptr);
    return desc.base + found->offset;
  }

  [[nodiscard]] auto read(std::string_view peripheral, std::string_view reg) const -> std::uint32_t
  {
    auto const value = f.target->mem().read(at(peripheral, reg), 4);
    REQUIRE(value.has_value());
    return static_cast<std::uint32_t>(*value);
  }

  void write(std::string_view peripheral, std::string_view reg, std::uint32_t value) const
  {
    REQUIRE(f.target->mem().write(at(peripheral, reg), 4, value).has_value());
  }

  void rcc_field(std::string_view reg, std::string_view name, std::uint32_t value) const
  {
    auto const& desc  = f.target->peripheral("RCC").description();
    auto const* found = popcorn::find_register(desc, reg);
    REQUIRE(found != nullptr);
    auto const bits = std::ranges::find(found->fields, name, &popcorn::field_desc::name);
    REQUIRE(bits != found->fields.end());
    auto const mask = ((std::uint32_t{1} << bits->width) - 1) << bits->lsb;
    write("RCC", reg, (read("RCC", reg) & ~mask) | (value << bits->lsb));
  }

  [[nodiscard]] auto count() const -> std::uint32_t
  {
    return read("WWDG", "CR") & t_mask;
  }

  [[nodiscard]] auto events(std::uint32_t kind) const -> std::vector<std::uint64_t>
  {
    auto const& units = f.target->description().peripherals;
    auto const  owner = static_cast<std::uint32_t>(
        std::ranges::find(units, std::string_view{"WWDG"}, &popcorn::peripheral_desc::name)
        - units.begin());
    std::vector<std::uint64_t> out;
    for(auto const& event : f.target->snapshot().data().events.pending)
    {
      if(event.id.owner == owner and event.id.kind == kind)
      {
        out.push_back(event.due_ps);
      }
    }
    return out;
  }

  [[nodiscard]] auto expiry_due() const -> std::optional<std::uint64_t>
  {
    auto const found = events(0);
    return found.size() == 1 ? std::optional{found.front()} : std::nullopt;
  }

  [[nodiscard]] auto early_due() const -> std::optional<std::uint64_t>
  {
    auto const found = events(1);
    return found.size() == 1 ? std::optional{found.front()} : std::nullopt;
  }

  void run_for(std::uint64_t ps) const
  {
    f.run_for(ps, icount{10'000'000});
  }

  void run_until(std::uint64_t ps) const
  {
    REQUIRE(
        f.run_to_stop({.max_instructions = icount{10'000'000}, .until_time = vtime{ps}}).reason
        == stop_reason::time);
  }

  [[nodiscard]] auto reset_requested(std::uint64_t ps) const -> bool
  {
    return f.run_to_stop({.max_instructions = icount{10'000'000}, .until_time = vtime{ps}}).reason
        == stop_reason::reset_request;
  }

  [[nodiscard]] auto ticks_at(std::uint64_t ps, std::uint64_t hz, std::uint32_t shift) const
      -> std::uint64_t
  {
    return cycles_in(ps - t0, hz) / period_cycles(shift);
  }

  [[nodiscard]] auto tick_time(std::uint64_t tick, std::uint64_t hz, std::uint32_t shift) const
      -> std::uint64_t
  {
    return t0 + ps_of(tick * period_cycles(shift), hz);
  }

  [[nodiscard]] auto
      expiry_after_write(std::uint32_t t, std::uint32_t shift, std::uint64_t hz, std::uint64_t at)
          const -> std::uint64_t
  {
    return tick_time(ticks_at(at, hz, shift) + t - 0x3F, hz, shift);
  }
};

inline auto with_cfr_changed(part_description desc, bool drop_register, std::string_view drop_field)
    -> part_description
{
  auto found = std::ranges::find(
      desc.peripherals, std::string_view{"WWDG"}, &popcorn::peripheral_desc::name);
  REQUIRE(found != desc.peripherals.end());
  if(drop_register)
  {
    std::erase_if(
        found->registers, [](popcorn::register_desc const& reg) { return reg.name == "CFR"; });
  }
  for(auto& reg : found->registers)
  {
    std::erase_if(
        reg.fields, [&](popcorn::field_desc const& one) { return one.name == drop_field; });
  }
  return desc;
}
}

namespace
{
namespace core = popcorn::core;
using popcorn::stop_reason;
using popcorn::testing::sym;
}

TEST_CASE("WWDG reset values: CR and CFR 0x7F, SR 0, the counter reads T")
{
  wwdg_test::bench b{wwdg_test::the_rig()};
  CHECK(b.read("WWDG", "CR") == 0x7F);
  CHECK(b.read("WWDG", "CFR") == 0x7F);
  CHECK(b.read("WWDG", "SR") == 0);
  CHECK_FALSE(b.expiry_due().has_value());
}

TEST_CASE("the WWDG expiry is the 64th tick after the reload, to the picosecond, per WDGTB")
{
  for(std::uint32_t shift = 0; shift < 4; ++shift)
  {
    CAPTURE(shift);
    wwdg_test::bench b{wwdg_test::the_rig()};
    b.write("WWDG", "CFR", 0x7F | shift << wwdg_test::shift_at | wwdg_test::ewi);
    b.run_for(3 * wwdg_test::ms);
    b.write("WWDG", "CR", 0xFF);
    auto const at  = b.f.now().ps;
    auto const due = b.expiry_due();
    REQUIRE(due.has_value());
    CHECK(*due == b.expiry_after_write(0x7F, shift, wwdg_test::the_rig().pclk1_hz, at));
    auto const early = b.early_due();
    REQUIRE(early.has_value());
    CHECK(
        *early
        == b.tick_time(
            b.ticks_at(at, wwdg_test::the_rig().pclk1_hz, shift) + 0x3F,
            wwdg_test::the_rig().pclk1_hz, shift));
    CHECK(b.reset_requested(*due - wwdg_test::ps_of(2, wwdg_test::the_rig().pclk1_hz)) == false);
    CHECK(b.reset_requested(*due + 1 * wwdg_test::ms));
    CHECK(b.f.now().ps >= *due);
  }
}

TEST_CASE("the WWDG expiry instant does not depend on the slicing of the run")
{
  std::uint64_t first = 0;
  for(int slices : {1, 40})
  {
    CAPTURE(slices);
    wwdg_test::bench b{wwdg_test::the_rig()};
    b.write("WWDG", "CR", 0xFF);
    auto const due = *b.expiry_due();
    for(int slice = 1; slice < slices; ++slice)
    {
      b.run_until(b.f.now().ps + (due - b.f.now().ps) / (slices - slice + 1));
    }
    REQUIRE(b.reset_requested(due + wwdg_test::ms));
    if(first == 0)
    {
      first = b.f.now().ps;
    }
    CHECK(b.f.now().ps == first);
  }
}

TEST_CASE("RM0351 37.3.3: the counter is free-running with WDGA 0 and wraps from 0 to 0x7F")
{
  wwdg_test::bench b{wwdg_test::the_rig()};
  auto const       hz     = wwdg_test::the_rig().pclk1_hz;
  auto const       target = b.tick_time(130, hz, 0) + 1;
  b.run_until(target);
  CHECK(b.read("WWDG", "CR") == ((0x7F + 128 - 130) % 128));
  CHECK(b.read("WWDG", "SR") == 0);
  CHECK_FALSE(b.expiry_due().has_value());
  b.write("WWDG", "CR", 0x50);
  auto const reloaded = b.ticks_at(b.f.now().ps, hz, 0);
  b.run_until(b.tick_time(reloaded + 0x50 + 3, hz, 0) + 1);
  CHECK(b.read("WWDG", "CR") == 0x7D);
  CHECK(b.read("WWDG", "SR") == 0);
  b.write("WWDG", "CR", 0xFF);
  CHECK_FALSE(b.reset_requested(b.f.now().ps + wwdg_test::ms));
}

TEST_CASE("RM0351 37.3.3: a CR write reloads T and leaves the divider phase alone (inferred)")
{
  wwdg_test::bench b{wwdg_test::the_rig()};
  auto const       hz = wwdg_test::the_rig().pclk1_hz;
  b.run_until(b.tick_time(10, hz, 0) + 1);
  b.write("WWDG", "CR", 0x6F);
  CHECK(b.count() == 0x6F);
  b.run_until(b.tick_time(12, hz, 0) - wwdg_test::ps_of(2, hz));
  CHECK(b.count() == 0x6E);
  b.run_until(b.tick_time(12, hz, 0));
  CHECK(b.count() == 0x6D);
}

TEST_CASE("RM0351 37.3 and Figure 387: a CR write resets when the counter before it is above W")
{
  auto const hz = wwdg_test::the_rig().pclk1_hz;
  {
    wwdg_test::bench b{wwdg_test::the_rig()};
    b.write("WWDG", "CR", 0xFF);
    b.write("WWDG", "CFR", 0x50);
    b.run_until(b.tick_time(5, hz, 0) + 1);
    b.write("WWDG", "CR", 0xFF);
    CHECK(b.reset_requested(b.f.now().ps + wwdg_test::ms));
  }
  {
    wwdg_test::bench b{wwdg_test::the_rig()};
    b.write("WWDG", "CR", 0xFF);
    b.run_until(b.tick_time(20, hz, 0) + 1);
    auto const live = b.count();
    b.write("WWDG", "CFR", live);
    b.write("WWDG", "CR", 0xFF);
    CHECK_FALSE(b.reset_requested(b.f.now().ps + wwdg_test::ms));
    CHECK(b.expiry_due().has_value());
  }
  {
    wwdg_test::bench b{wwdg_test::the_rig()};
    b.write("WWDG", "CR", 0xFF);
    b.run_until(b.tick_time(20, hz, 0) + 1);
    auto const live = b.count();
    b.write("WWDG", "CFR", live - 1);
    b.write("WWDG", "CR", 0xFF);
    CHECK(b.reset_requested(b.f.now().ps + wwdg_test::ms));
  }
  {
    wwdg_test::bench b{wwdg_test::the_rig()};
    b.write("WWDG", "CR", 0xFF);
    b.write("WWDG", "CFR", 0x50);
    b.run_until(b.tick_time(50, hz, 0) + 1);
    REQUIRE(b.count() < 0x50);
    b.write("WWDG", "CR", 0xFF);
    CHECK_FALSE(b.reset_requested(b.f.now().ps + wwdg_test::ms));
  }
}

TEST_CASE("a CR write with T 0x40 sets no EWIF and expires one tick later (inferred)")
{
  auto const       hz = wwdg_test::the_rig().pclk1_hz;
  wwdg_test::bench b{wwdg_test::the_rig()};
  b.write("WWDG", "CFR", 0x27F);
  b.run_for(3 * wwdg_test::ms);
  b.write("WWDG", "CR", 0xC0);
  auto const base = b.ticks_at(b.f.now().ps, hz, 0);
  CHECK(b.count() == 0x40);
  CHECK(b.read("WWDG", "SR") == 0);
  CHECK_FALSE(b.early_due().has_value());
  REQUIRE(b.expiry_due().has_value());
  CHECK(*b.expiry_due() == b.tick_time(base + 1, hz, 0));
  b.run_until(*b.expiry_due() - wwdg_test::ps_of(2, hz));
  CHECK(b.read("WWDG", "SR") == 0);
  CHECK_FALSE(b.f.pending(wwdg_test::the_rig().irq));
}

TEST_CASE("the window compare applies to the CR write that sets WDGA (inferred)")
{
  auto const hz = wwdg_test::the_rig().pclk1_hz;
  {
    wwdg_test::bench b{wwdg_test::the_rig()};
    b.write("WWDG", "CFR", 0x50);
    b.run_until(b.tick_time(5, hz, 0) + 1);
    REQUIRE(b.count() > 0x50);
    b.write("WWDG", "CR", 0xFF);
    CHECK(b.reset_requested(b.f.now().ps + wwdg_test::ms));
  }
  {
    wwdg_test::bench b{wwdg_test::the_rig()};
    b.write("WWDG", "CFR", 0x50);
    b.run_until(b.tick_time(5, hz, 0) + 1);
    b.write("WWDG", "CR", 0x7F);
    CHECK_FALSE(b.reset_requested(b.f.now().ps + wwdg_test::ms));
  }
}

TEST_CASE("RM0351 37.3.3 Note: a CR write with WDGA and T6 clear resets at once")
{
  {
    wwdg_test::bench b{wwdg_test::the_rig()};
    b.write("WWDG", "CR", 0xBF);
    CHECK(b.reset_requested(b.f.now().ps + wwdg_test::ms));
  }
  {
    wwdg_test::bench b{wwdg_test::the_rig()};
    b.write("WWDG", "CR", 0xFF);
    b.write("WWDG", "CR", 0xBF);
    CHECK(b.reset_requested(b.f.now().ps + wwdg_test::ms));
  }
  {
    wwdg_test::bench b{wwdg_test::the_rig()};
    b.write("WWDG", "CR", 0x3F);
    CHECK(b.count() == 0x3F);
    CHECK_FALSE(b.reset_requested(b.f.now().ps + 100 * wwdg_test::ms));
  }
}

TEST_CASE("RM0351 37.5.1 and 37.5.2: WDGA and EWI stay set until a reset")
{
  wwdg_test::bench b{wwdg_test::the_rig()};
  b.write("WWDG", "CR", 0xFF);
  b.write("WWDG", "CFR", 0x27F);
  b.write("WWDG", "CR", 0x7F);
  b.write("WWDG", "CFR", 0x7F);
  CHECK((b.read("WWDG", "CR") & wwdg_test::wdga) == wwdg_test::wdga);
  CHECK((b.read("WWDG", "CFR") & wwdg_test::ewi) == wwdg_test::ewi);
  CHECK(b.expiry_due().has_value());
}

TEST_CASE(
    "RM0351 37.4: EWIF sets at 0x40 with the IRQ when EWI is set, rc_w0, the reset a tick later")
{
  auto const       hz = wwdg_test::the_rig().pclk1_hz;
  wwdg_test::bench b{wwdg_test::the_rig()};
  b.write("WWDG", "CFR", 0x27F);
  b.write("WWDG", "CR", 0xFF);
  auto const early = *b.early_due();
  auto const last  = *b.expiry_due();
  CHECK(last == b.tick_time(b.ticks_at(early, hz, 0) + 1, hz, 0));
  b.run_until(early - wwdg_test::ps_of(2, hz));
  CHECK(b.read("WWDG", "SR") == 0);
  CHECK_FALSE(b.f.pending(wwdg_test::the_rig().irq));
  b.run_until(early + 1);
  CHECK(b.read("WWDG", "SR") == 1);
  CHECK(b.f.pending(wwdg_test::the_rig().irq));
  CHECK(b.count() == 0x40);
  b.write("WWDG", "SR", 1);
  CHECK(b.read("WWDG", "SR") == 1);
  b.write("WWDG", "SR", 0);
  CHECK(b.read("WWDG", "SR") == 0);
  CHECK(b.reset_requested(last + wwdg_test::ms));
}

TEST_CASE("RM0351 37.5.3: EWIF is also set with EWI clear and raises no IRQ")
{
  wwdg_test::bench b{wwdg_test::the_rig()};
  b.write("WWDG", "CR", 0xFF);
  b.run_until(*b.early_due() + 1);
  CHECK(b.read("WWDG", "SR") == 1);
  CHECK_FALSE(b.f.pending(wwdg_test::the_rig().irq));
}

TEST_CASE("a PCLK1 rate change keeps the partial division and moves the expiry")
{
  auto const       hz  = wwdg_test::the_rig().pclk1_hz;
  auto const       hz2 = hz / 2;
  wwdg_test::bench b{wwdg_test::the_rig()};
  b.run_for(2 * wwdg_test::ms);
  b.write("WWDG", "CR", 0xFF);
  auto const written = b.f.now().ps;
  b.run_for(5 * wwdg_test::ms);
  b.rcc_field("CFGR", "PPRE1", 4);
  auto const changed = b.f.now().ps;
  auto const done    = wwdg_test::cycles_in(changed - b.t0, hz);
  auto const target  = (wwdg_test::cycles_in(written - b.t0, hz) / 4096 + 0x7F - 0x3F) * 4096;
  auto const due     = b.expiry_due();
  REQUIRE(due.has_value());
  CHECK(*due == changed + wwdg_test::ps_of(target - done, hz2));
}

TEST_CASE("a WDGTB change keeps the counter and the divider count, the tap moves (inferred)")
{
  auto const       hz = wwdg_test::the_rig().pclk1_hz;
  wwdg_test::bench b{wwdg_test::the_rig()};
  b.write("WWDG", "CR", 0xFF);
  b.run_until(b.tick_time(10, hz, 0) + 5 * wwdg_test::ms);
  auto const before = b.count();
  auto const at     = b.f.now().ps;
  b.write("WWDG", "CFR", 0x7F | 2u << wwdg_test::shift_at);
  CHECK(b.count() == before);
  auto const base = wwdg_test::cycles_in(at - b.t0, hz) / wwdg_test::period_cycles(2);
  CHECK(*b.expiry_due() == b.tick_time(base + before - 0x3F, hz, 2));
}

TEST_CASE("RCC WWDGEN clear freezes the counter, the enable restarts the divider at 0 (inferred)")
{
  auto const       hz = wwdg_test::the_rig().pclk1_hz;
  wwdg_test::bench b{wwdg_test::the_rig()};
  b.write("WWDG", "CR", 0xFF);
  b.run_until(b.tick_time(6, hz, 0) + 1);
  auto const frozen = b.count();
  b.rcc_field(wwdg_test::the_rig().enable_reg, "WWDGEN", 0);
  CHECK_FALSE(b.expiry_due().has_value());
  CHECK_FALSE(b.early_due().has_value());
  b.run_for(2'000 * wwdg_test::ms);
  auto const resumed = b.f.now().ps;
  b.enable_clock();
  CHECK(b.count() == frozen);
  CHECK(*b.expiry_due() == resumed + wwdg_test::ps_of((frozen - 0x3F) * 4096, hz));
}

TEST_CASE("1000 CR writes leave at most one expiry and one early wake-up pending")
{
  wwdg_test::bench b{wwdg_test::the_rig()};
  b.write("WWDG", "CR", 0xFF);
  for(int write = 0; write < 1000; ++write)
  {
    b.write("WWDG", "CR", 0xFF);
  }
  CHECK(b.events(0).size() == 1);
  CHECK(b.events(1).size() == 1);
}

TEST_CASE("a snapshot mid-count restores to the same WWDG expiry")
{
  wwdg_test::bench b{wwdg_test::the_rig()};
  b.write("WWDG", "CFR", 0x27F);
  b.write("WWDG", "CR", 0xFF);
  b.run_for(30 * wwdg_test::ms);
  auto const saved = b.f.target->snapshot();
  auto const due   = *b.expiry_due();
  REQUIRE(b.reset_requested(due + wwdg_test::ms));
  auto const stopped = b.f.now();

  wwdg_test::bench restored{wwdg_test::the_rig()};
  restored.f.target->restore(
      core::snapshot{core::decode_snapshot(core::encode_snapshot(saved.data()))});
  CHECK(*restored.expiry_due() == due);
  REQUIRE(restored.reset_requested(due + wwdg_test::ms));
  CHECK(restored.f.now() == stopped);
}

TEST_CASE("checklist 39: the WWDG row needs CFR and the WDGTB field")
{
  CHECK_NOTHROW(
      popcorn::testing::stm32_fixture(wwdg_test::the_rig().desc(), wwdg_test::the_rig().firmware));
  CHECK_THROWS_AS(
      popcorn::testing::stm32_fixture(
          wwdg_test::with_cfr_changed(wwdg_test::the_rig().desc(), true, ""),
          wwdg_test::the_rig().firmware),
      popcorn::config_error);
  CHECK_THROWS_AS(
      popcorn::testing::stm32_fixture(
          wwdg_test::with_cfr_changed(wwdg_test::the_rig().desc(), false, "WDGTB"),
          wwdg_test::the_rig().firmware),
      popcorn::config_error);
}

TEST_CASE("under reset_and_continue the LL firmware restarts and reads RCC_CSR.WWDGRSTF")
{
  auto options             = popcorn::testing::stm32_options();
  options.on_reset_request = core::reset_request_policy::reset_and_continue;
  popcorn::testing::stm32_fixture f{
      wwdg_test::the_rig().desc(), wwdg_test::the_rig().firmware, std::move(options)};
  auto const stopped = f.run_to_stop();
  CAPTURE(stopped.detail);
  REQUIRE(stopped.reason == stop_reason::user);
  CHECK(f.log() == std::vector<std::uint32_t>{1});
  CHECK(f.field("RCC", "CSR", "WWDGRSTF") == 1);
}

TEST_CASE("the LL firmware arms WWDG with WDGTB 3, the last refresh plus 64 ticks resets")
{
  popcorn::testing::stm32_fixture f{wwdg_test::the_rig().desc(), wwdg_test::the_rig().firmware};
  std::vector<std::uint64_t>      refreshes;
  auto const                      hook = f.target->add_intercept(
      sym(wwdg_test::the_rig().firmware, "refresh"), [&](core::intercept_call const& c) {
        refreshes.push_back(c.target().virtual_clock().time().ps);
        return core::intercept_result::proceed();
      });
  auto const stopped = f.run_to_stop();
  REQUIRE(stopped.reason == stop_reason::reset_request);
  REQUIRE(refreshes.size() == 50);
  auto const tick = wwdg_test::ps_of(wwdg_test::period_cycles(3), wwdg_test::the_rig().pclk1_hz);
  CHECK(f.log() == std::vector<std::uint32_t>{0, 0xFF, 0x1FF});
  CHECK(f.now().ps > refreshes.back() + 63 * tick);
  CHECK(f.now().ps <= refreshes.back() + 64 * tick + 10 * wwdg_test::ms / 1000);
}
