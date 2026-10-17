#include "../../../native/src/core/machine_impl.hpp"
#include "gtzc.hpp"
#include "stm32_fixture.hpp"

#include <popcorn/arch/armv8m/armv8m.hpp>
#include <popcorn/core/errors.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/periph/model.hpp>
#include <popcorn/periph/stm32/board.hpp>
#include <popcorn/periph/stm32/builtin.hpp>

#include <doctest/doctest.h>

#include <unicorn/unicorn.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <initializer_list>
#include <memory>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace
{
namespace core   = popcorn::core;
namespace periph = popcorn::periph;
namespace stm32  = popcorn::periph::stm32;
using popcorn::address;
using popcorn::bus_attrs;
using popcorn::icount;
using popcorn::initiator;
using popcorn::testing::u575;

constexpr bus_attrs secure_priv{.by = initiator::cpu, .secure = true, .privileged = true};
constexpr bus_attrs secure_unpriv{.by = initiator::cpu, .secure = true, .privileged = false};
constexpr bus_attrs nonsecure_priv{.by = initiator::cpu, .secure = false, .privileged = true};
constexpr bus_attrs nonsecure_unpriv{.by = initiator::cpu, .secure = false, .privileged = false};

class probe final : public periph::model
{
public:
  explicit probe(periph::model_context& ctx) : ctx_{ctx}
  {}

  [[nodiscard]] auto claims() const -> std::vector<std::string> override
  {
    return {};
  }

  void reset(periph::register_file&, popcorn::reset_kind) override
  {}

  auto on_read(periph::register_file&, std::uint32_t, std::uint8_t, periph::access_ctx)
      -> std::expected<std::uint32_t, popcorn::bus_error> override
  {
    return 0;
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

  [[nodiscard]] auto services() const -> core::machine_services&
  {
    return ctx_.services;
  }

private:
  periph::model_context& ctx_;
};

auto unit(std::string_view name) -> popcorn::peripheral_desc const&
{
  return *std::ranges::find(u575().peripherals, name, &popcorn::peripheral_desc::name);
}

auto address_of(std::string_view peripheral, std::string_view reg) -> address
{
  auto const& p     = unit(peripheral);
  auto const* found = popcorn::find_register(p, reg);
  REQUIRE(found != nullptr);
  return p.base + found->offset;
}

auto bit(std::string_view peripheral, std::string_view reg, std::string_view field) -> std::uint32_t
{
  auto const* found = popcorn::find_register(unit(peripheral), reg);
  REQUIRE(found != nullptr);
  auto const bits = std::ranges::find(found->fields, field, &popcorn::field_desc::name);
  REQUIRE(bits != found->fields.end());
  return std::uint32_t{1} << bits->lsb;
}

struct rig
{
  std::unique_ptr<core::machine> target;
  probe*                         bus{};

  explicit rig(
      bool                        trustzone,
      stm32::option_bytes         wanted       = {.tzen = true},
      popcorn::arch::fault_policy faults       = popcorn::arch::fault_policy::stop,
      bool                        debug_blocks = true,
      bool                        strict       = false)
  {
    stm32::register_builtin_models();
    auto options               = popcorn::testing::stm32_options();
    options.faults             = faults;
    options.strict_peripherals = strict;
    if(debug_blocks)
    {
      options.raz_wi = stm32::raz_wi_map(u575());
    }
    if(trustzone)
    {
      wanted.tzen = true;
      REQUIRE(stm32::configure_option_bytes(u575(), wanted, options).has_value());
    }
    options.models.emplace(
        "LPGPIO1", [this](periph::model_context& ctx, popcorn::peripheral_desc const&) {
          auto made = std::make_shared<probe>(ctx);
          bus       = made.get();
          return made;
        });
    target = std::make_unique<core::machine>(u575(), popcorn::arch::armv8m::factory(), options);
    target->reset();
  }

  auto
      write(std::string_view peripheral, std::string_view reg, std::uint32_t value, bus_attrs attrs)
          const -> void
  {
    REQUIRE(bus->services().write(address_of(peripheral, reg), 4, value, attrs).has_value());
  }

  [[nodiscard]] auto read(std::string_view peripheral, std::string_view reg, bus_attrs attrs) const
      -> std::uint32_t
  {
    auto const value = bus->services().read(address_of(peripheral, reg), 4, attrs);
    REQUIRE(value.has_value());
    return *value;
  }

  [[nodiscard]] auto peek(std::string_view peripheral, std::string_view reg) const -> std::uint32_t
  {
    auto const* found = popcorn::find_register(unit(peripheral), reg);
    REQUIRE(found != nullptr);
    return target->peripheral(peripheral).peek(found->offset & ~3u);
  }

  [[nodiscard]] auto irq_pending(std::uint32_t irq) const -> bool
  {
    auto const word = target->mem().read(0xe000'e200u + 4 * (irq / 32), 4);
    REQUIRE(word.has_value());
    return (*word >> (irq % 32) & 1u) != 0;
  }
};

constexpr std::uint32_t gtzc_irq = 8;

TEST_CASE("gtzc_fields maps every TZSC SECCFGRx and TZIC IERx field of the U575")
{
  auto const            fields = stm32::gtzc_fields(u575());
  std::set<std::string> unknown;
  std::set<std::string> memory;
  for(auto const& f : fields)
  {
    auto const name = f.owner + "." + f.field;
    if(f.target == stm32::gtzc_target::unknown)
    {
      unknown.insert(name);
    }
    if(f.target == stm32::gtzc_target::memory)
    {
      memory.insert(name);
    }
  }
  CHECK(
      unknown
      == std::set<std::string>{
          "GTZC1_TZIC.AESIE", "GTZC1_TZIC.SAESIE", "GTZC1_TZSC.AESSEC", "GTZC1_TZSC.SAESSEC"});
  CHECK(
      memory
      == std::set<std::string>{
          "GTZC1_TZIC.BKPSRAMIE", "GTZC1_TZIC.FLASHIE", "GTZC1_TZIC.FSMC_MEMIE",
          "GTZC1_TZIC.OCTOSPI1_MEMIE", "GTZC1_TZIC.OCTOSPI2_MEMIE", "GTZC1_TZIC.SRAM1IE",
          "GTZC1_TZIC.SRAM2IE", "GTZC1_TZIC.SRAM3IE", "GTZC2_TZIC.SRAM4IE"});
  auto const all_named = [&](std::string_view owner, std::string_view field) {
    auto const found = std::ranges::find_if(
        fields, [&](stm32::gtzc_field const& f) { return f.owner == owner and f.field == field; });
    REQUIRE(found != fields.end());
    std::vector<std::string> names;
    for(auto const unit : found->units)
    {
      names.push_back(u575().peripherals[unit].name);
    }
    return names;
  };
  auto const named = [&](std::string_view owner, std::string_view field) {
    auto const names = all_named(owner, field);
    REQUIRE(names.size() == 1);
    return names.front();
  };
  using names = std::vector<std::string>;
  CHECK(all_named("GTZC1_TZSC", "DCMISEC") == names{"DCMI", "PSSI"});
  CHECK(all_named("GTZC1_TZIC", "DCMIIE") == names{"DCMI", "PSSI"});
  CHECK(all_named("GTZC1_TZSC", "ADC1SEC") == names{"ADC1", "ADC12"});
  CHECK(all_named("GTZC1_TZIC", "ADC1IE") == names{"ADC1", "ADC12"});
  CHECK(all_named("GTZC1_TZSC", "FDCAN1SEC") == names{"FDCAN1", "FDCAN1_RAM"});
  CHECK(all_named("GTZC1_TZSC", "SDMMC1SEC") == names{"SDMMC1", "DLYBSD1"});
  CHECK(all_named("GTZC1_TZSC", "SDMMC2SEC") == names{"SDMMC2", "DLYBSD2"});
  CHECK(all_named("GTZC1_TZSC", "OCTOSPI1_REGSEC") == names{"OCTOSPI1", "DLYBOS1"});
  CHECK(all_named("GTZC1_TZIC", "OCTOSPI2IE") == names{"OCTOSPI2", "DLYBOS2"});
  CHECK(named("GTZC1_TZSC", "ICACHE_REGSEC") == "ICache");
  CHECK(named("GTZC1_TZSC", "OTGFSSEC") == "OTG_FS");
  CHECK(named("GTZC1_TZSC", "FSMC_REGSEC") == "FMC");
  CHECK(named("GTZC1_TZIC", "USART4IE") == "UART4");
  CHECK(named("GTZC1_TZIC", "FLASH_REGIE") == "FLASH");
  CHECK(named("GTZC1_TZIC", "TZSC1IE") == "GTZC1_TZSC");
  CHECK(named("GTZC1_TZIC", "MPCBB3_REGIE") == "GTZC1_MPCBB3");
  CHECK(named("GTZC2_TZIC", "MPCBB4_REGIE") == "GTZC2_MPCBB4");
  CHECK(named("GTZC2_TZSC", "LPUART1SEC") == "LPUART1");
  for(auto const& sec : fields)
  {
    if(sec.target != stm32::gtzc_target::peripheral or not sec.owner.ends_with("_TZSC"))
    {
      continue;
    }
    auto const tzic = sec.owner.substr(0, sec.owner.size() - 4) + "TZIC";
    CAPTURE(sec.field);
    CHECK(std::ranges::any_of(fields, [&](stm32::gtzc_field const& ie) {
      return ie.owner == tzic and ie.units == sec.units;
    }));
  }
}

TEST_CASE(
    "RM0456 5.4.2: a Non-secure access to a Secure TIM2 is RAZ/WI with a TZIC event and IRQ 8")
{
  rig const r{true};
  r.write("TIM2", "CR1", 0x80, nonsecure_priv);
  CHECK(r.peek("TIM2", "CR1") == 0x80);
  r.write("GTZC1_TZSC", "SECCFGR1", bit("GTZC1_TZSC", "SECCFGR1", "TIM2SEC"), secure_priv);
  CHECK(r.peek("GTZC1_TZSC", "SECCFGR1") == bit("GTZC1_TZSC", "SECCFGR1", "TIM2SEC"));
  CHECK(r.peek("GTZC1_TZIC", "SR1") == 0);

  r.write("TIM2", "CR1", 0x1, nonsecure_priv);
  CHECK(r.peek("TIM2", "CR1") == 0x80);
  CHECK(r.read("TIM2", "CR1", nonsecure_priv) == 0);
  auto const flag = bit("GTZC1_TZIC", "SR1", "TIM2F");
  CHECK(r.peek("GTZC1_TZIC", "SR1") == flag);
  CHECK_FALSE(r.irq_pending(gtzc_irq));

  r.write("GTZC1_TZIC", "IER1", bit("GTZC1_TZIC", "IER1", "TIM2IE"), secure_priv);
  CHECK(r.irq_pending(gtzc_irq));
  CHECK(r.read("GTZC1_TZIC", "SR1", secure_priv) == flag);
  r.write("GTZC1_TZIC", "FCR1", flag, secure_priv);
  CHECK(r.peek("GTZC1_TZIC", "SR1") == 0);

  r.write("TIM2", "CR1", 0x81, secure_priv);
  CHECK(r.peek("TIM2", "CR1") == 0x81);
  CHECK(r.read("TIM2", "CR1", secure_priv) == 0x81);
  CHECK(r.peek("GTZC1_TZIC", "SR1") == 0);
}

TEST_CASE("RM0456 3.5.5 Table 11: a Non-secure DMA access to a Secure TIM2 is RAZ/WI with an event")
{
  rig const r{true};
  r.write("GTZC1_TZSC", "SECCFGR1", bit("GTZC1_TZSC", "SECCFGR1", "TIM2SEC"), secure_priv);
  r.write("TIM2", "CR1", 0x1, bus_attrs{.by = initiator::dma, .secure = false});
  CHECK(r.peek("TIM2", "CR1") == 0);
  CHECK(r.peek("GTZC1_TZIC", "SR1") == bit("GTZC1_TZIC", "SR1", "TIM2F"));
  r.write("TIM2", "CR1", 0x1, bus_attrs{.by = initiator::dma, .secure = true});
  CHECK(r.peek("TIM2", "CR1") == 0x1);
}

TEST_CASE("RM0456 5.4.2: an unprivileged access to a privileged TIM2 is RAZ/WI with no event")
{
  rig const r{true};
  r.write("TIM2", "CR1", 0x80, secure_priv);
  r.write("GTZC1_TZSC", "PRIVCFGR1", bit("GTZC1_TZSC", "PRIVCFGR1", "TIM2PRIV"), nonsecure_priv);
  CHECK(r.peek("GTZC1_TZSC", "PRIVCFGR1") == bit("GTZC1_TZSC", "PRIVCFGR1", "TIM2PRIV"));
  CHECK(r.read("TIM2", "CR1", nonsecure_unpriv) == 0);
  CHECK(r.read("TIM2", "CR1", secure_unpriv) == 0);
  r.write("TIM2", "CR1", 0x1, secure_unpriv);
  CHECK(r.peek("TIM2", "CR1") == 0x80);
  CHECK(r.read("TIM2", "CR1", nonsecure_priv) == 0x80);
  CHECK(r.peek("GTZC1_TZIC", "SR1") == 0);
}

TEST_CASE("RM0456 5.6.1: TZSC CR.LCK ignores SECCFGR and PRIVCFGR writes until a reset")
{
  rig const  r{true};
  auto const tim2 = bit("GTZC1_TZSC", "SECCFGR1", "TIM2SEC");
  r.write("GTZC1_TZSC", "CR", 1, secure_unpriv);
  CHECK(r.peek("GTZC1_TZSC", "CR") == 0);
  r.write("GTZC1_TZSC", "CR", 1, secure_priv);
  CHECK(r.peek("GTZC1_TZSC", "CR") == 1);
  r.write("GTZC1_TZSC", "CR", 0, secure_priv);
  CHECK(r.peek("GTZC1_TZSC", "CR") == 1);
  r.write("GTZC1_TZSC", "SECCFGR1", tim2, secure_priv);
  r.write("GTZC1_TZSC", "PRIVCFGR1", tim2, secure_priv);
  CHECK(r.peek("GTZC1_TZSC", "SECCFGR1") == 0);
  CHECK(r.peek("GTZC1_TZSC", "PRIVCFGR1") == 0);
  r.target->reset(popcorn::reset_kind::software);
  CHECK(r.peek("GTZC1_TZSC", "CR") == 0);
  r.write("GTZC1_TZSC", "SECCFGR1", tim2, secure_priv);
  CHECK(r.peek("GTZC1_TZSC", "SECCFGR1") == tim2);
}

TEST_CASE("RM0456 5.4.1 and 5.6.2: the TZSC write rules of the SEC and PRIV bits")
{
  rig const  r{true};
  auto const tim2_sec  = bit("GTZC1_TZSC", "SECCFGR1", "TIM2SEC");
  auto const tim3_sec  = bit("GTZC1_TZSC", "SECCFGR1", "TIM3SEC");
  auto const tim2_priv = bit("GTZC1_TZSC", "PRIVCFGR1", "TIM2PRIV");
  auto const tim3_priv = bit("GTZC1_TZSC", "PRIVCFGR1", "TIM3PRIV");
  auto const tzsc_flag = bit("GTZC1_TZIC", "SR4", "TZSC1F");

  r.write("GTZC1_TZSC", "SECCFGR1", tim2_sec, nonsecure_priv);
  CHECK(r.peek("GTZC1_TZSC", "SECCFGR1") == 0);
  CHECK(r.peek("GTZC1_TZIC", "SR4") == tzsc_flag);
  CHECK(r.read("GTZC1_TZSC", "SECCFGR1", nonsecure_unpriv) == 0);
  CHECK(r.read("GTZC1_TZSC", "CR", nonsecure_priv) == 0);
  r.write("GTZC1_TZIC", "FCR4", tzsc_flag, secure_priv);

  r.write("GTZC1_TZSC", "PRIVCFGR1", tim2_priv, secure_priv);
  r.write("GTZC1_TZSC", "SECCFGR1", tim2_sec | tim3_sec, secure_unpriv);
  CHECK(r.peek("GTZC1_TZSC", "SECCFGR1") == tim3_sec);
  CHECK(r.read("GTZC1_TZSC", "SECCFGR1", nonsecure_unpriv) == tim3_sec);
  r.write("GTZC1_TZSC", "SECCFGR1", tim2_sec | tim3_sec, secure_priv);
  CHECK(r.peek("GTZC1_TZSC", "SECCFGR1") == (tim2_sec | tim3_sec));
  CHECK(r.peek("GTZC1_TZIC", "SR4") == 0);

  r.write("GTZC1_TZSC", "PRIVCFGR1", tim3_priv, secure_unpriv);
  CHECK(r.peek("GTZC1_TZSC", "PRIVCFGR1") == tim2_priv);
  r.write("GTZC1_TZSC", "SECCFGR1", tim2_sec, secure_priv);
  r.write("GTZC1_TZSC", "PRIVCFGR1", tim3_priv, nonsecure_priv);
  CHECK(r.peek("GTZC1_TZSC", "PRIVCFGR1") == (tim2_priv | tim3_priv));
  r.write("GTZC1_TZSC", "SECCFGR1", tim2_sec | tim3_sec, secure_priv);
  r.write("GTZC1_TZSC", "PRIVCFGR1", 0, nonsecure_priv);
  CHECK(r.peek("GTZC1_TZSC", "PRIVCFGR1") == (tim2_priv | tim3_priv));
  r.write("GTZC1_TZSC", "PRIVCFGR1", 0, secure_priv);
  CHECK(r.peek("GTZC1_TZSC", "PRIVCFGR1") == 0);
}

TEST_CASE("RM0456 5.7: TZIC registers are Secure privileged only, a Non-secure access flags TZIC1F")
{
  rig const  r{true};
  auto const tim2_ie = bit("GTZC1_TZIC", "IER1", "TIM2IE");
  auto const own     = bit("GTZC1_TZIC", "SR4", "TZIC1F");
  r.write("GTZC1_TZIC", "IER1", tim2_ie, secure_unpriv);
  CHECK(r.peek("GTZC1_TZIC", "IER1") == 0);
  CHECK(r.peek("GTZC1_TZIC", "SR4") == 0);
  r.write("GTZC1_TZIC", "IER1", tim2_ie, nonsecure_priv);
  CHECK(r.peek("GTZC1_TZIC", "IER1") == 0);
  CHECK(r.peek("GTZC1_TZIC", "SR4") == own);
  r.write("GTZC1_TZIC", "IER1", tim2_ie, secure_priv);
  CHECK(r.read("GTZC1_TZIC", "IER1", secure_priv) == tim2_ie);
  CHECK(r.read("GTZC1_TZIC", "IER1", secure_unpriv) == 0);
  CHECK(r.read("GTZC1_TZIC", "IER1", nonsecure_priv) == 0);
  CHECK(r.read("GTZC1_TZIC", "FCR4", secure_priv) == 0);
  r.write("GTZC1_TZIC", "SR1", tim2_ie, secure_priv);
  CHECK(r.peek("GTZC1_TZIC", "SR1") == 0);
  r.write("GTZC1_TZIC", "IER4", own, secure_priv);
  CHECK(r.irq_pending(gtzc_irq));
}

TEST_CASE("RM0456 5.4.2: GTZC2 secures LPUART1, with the event in GTZC2_TZIC")
{
  rig const r{true};
  r.write("LPUART1", "CR1", 0x8, secure_priv);
  r.write("GTZC2_TZSC", "SECCFGR1", bit("GTZC2_TZSC", "SECCFGR1", "LPUART1SEC"), secure_priv);
  r.write("LPUART1", "CR1", 0x0, nonsecure_priv);
  CHECK(r.peek("LPUART1", "CR1") == 0x8);
  CHECK(r.peek("GTZC2_TZIC", "SR1") == bit("GTZC2_TZIC", "SR1", "LPUART1F"));
  CHECK(r.peek("GTZC1_TZIC", "SR1") == 0);
  CHECK(r.peek("GTZC1_TZIC", "SR2") == 0);
  r.write("GTZC2_TZIC", "IER1", bit("GTZC2_TZIC", "IER1", "LPUART1IE"), secure_priv);
  CHECK(r.irq_pending(gtzc_irq));
  r.write("TIM2", "CR1", 0x1, nonsecure_priv);
  CHECK(r.peek("TIM2", "CR1") == 0x1);
}

auto superblocks(std::string_view mpcbb) -> std::uint32_t
{
  auto const g = stm32::mpcbb_geometry(u575(), mpcbb);
  REQUIRE(g.has_value());
  return g->size / (g->block_size * g->superblock_blocks);
}

TEST_CASE("RM0456 5.4.6 and 5.8.4: MPCBB SECCFGRx and PRIVCFGRx reset to all ones under TrustZone")
{
  rig const r{true};
  for(auto const name : {"GTZC1_MPCBB1", "GTZC1_MPCBB2", "GTZC1_MPCBB3", "GTZC2_MPCBB4"})
  {
    CAPTURE(name);
    for(std::uint32_t index = 0; index < superblocks(name); ++index)
    {
      CAPTURE(index);
      CHECK(r.read(name, std::format("SECCFGR{}", index), nonsecure_unpriv) == 0xffff'ffff);
      CHECK(r.read(name, std::format("PRIVCFGR{}", index), nonsecure_unpriv) == 0xffff'ffff);
    }
  }
  CHECK(superblocks("GTZC1_MPCBB1") == 12);
  CHECK(superblocks("GTZC1_MPCBB2") == 4);
  CHECK(superblocks("GTZC1_MPCBB3") == 32);
  CHECK(superblocks("GTZC2_MPCBB4") == 1);
}

TEST_CASE(
    "P1 row 3 kept at the SVD value: MPCBB SECCFGRx and PRIVCFGRx reset to all ones without "
    "TrustZone")
{
  rig const r{false};
  for(auto const name : {"GTZC1_MPCBB1", "GTZC1_MPCBB2", "GTZC1_MPCBB3", "GTZC2_MPCBB4"})
  {
    CAPTURE(name);
    for(auto const& described : unit(name).registers)
    {
      if(described.name.starts_with("SECCFGR") or described.name.starts_with("PRIVCFGR"))
      {
        CAPTURE(described.name);
        CHECK(r.peek(name, described.name) == 0xffff'ffff);
      }
    }
  }
  r.write("GTZC1_MPCBB1", "SECCFGR40", 0x5, nonsecure_unpriv);
  CHECK(r.peek("GTZC1_MPCBB1", "SECCFGR40") == 0x5);
}

TEST_CASE("RM0456 5.8.2: CFGLOCKR1 SPLCK0 locks super-block 0 only")
{
  rig const r{true};
  r.write("GTZC1_MPCBB1", "CFGLOCKR1", 1, secure_priv);
  CHECK(r.peek("GTZC1_MPCBB1", "CFGLOCKR1") == 1);
  r.write("GTZC1_MPCBB1", "CFGLOCKR1", 0, secure_priv);
  CHECK(r.peek("GTZC1_MPCBB1", "CFGLOCKR1") == 1);
  r.write("GTZC1_MPCBB1", "SECCFGR0", 0, secure_priv);
  r.write("GTZC1_MPCBB1", "PRIVCFGR0", 0, secure_priv);
  r.write("GTZC1_MPCBB1", "SECCFGR1", 0, secure_priv);
  CHECK(r.peek("GTZC1_MPCBB1", "SECCFGR0") == 0xffff'ffff);
  CHECK(r.peek("GTZC1_MPCBB1", "PRIVCFGR0") == 0xffff'ffff);
  CHECK(r.peek("GTZC1_MPCBB1", "SECCFGR1") == 0);
  r.target->reset(popcorn::reset_kind::pin);
  CHECK(r.peek("GTZC1_MPCBB1", "CFGLOCKR1") == 0);
  CHECK(r.peek("GTZC1_MPCBB1", "SECCFGR1") == 0xffff'ffff);
}

TEST_CASE("RM0456 5.3 Table 33: the U575 MPCBB1 has 12 super-blocks, SECCFGR12 is RAZ/WI")
{
  rig const r{true};
  r.write("GTZC1_MPCBB1", "SECCFGR11", 0x3, secure_priv);
  CHECK(r.peek("GTZC1_MPCBB1", "SECCFGR11") == 0x3);
  CHECK(r.read("GTZC1_MPCBB1", "SECCFGR12", secure_priv) == 0);
  r.write("GTZC1_MPCBB1", "SECCFGR12", 0x3, secure_priv);
  CHECK(r.peek("GTZC1_MPCBB1", "SECCFGR12") == 0);
  r.write("GTZC1_MPCBB1", "CFGLOCKR1", 0xffff'ffff, secure_priv);
  CHECK(r.peek("GTZC1_MPCBB1", "CFGLOCKR1") == 0xfff);
}

TEST_CASE("RM0456 5.8.4 and 5.8.5: the MPCBB write rules and the block bits")
{
  rig const   r{true};
  auto const* blocks = dynamic_cast<stm32::sram_block_controller*>(r.target->model("GTZC1_MPCBB1"));
  REQUIRE(blocks != nullptr);
  auto const own = bit("GTZC1_TZIC", "SR4", "MPCBB1_REGF");

  r.write("GTZC1_MPCBB1", "SECCFGR0", 0, nonsecure_priv);
  CHECK(r.peek("GTZC1_MPCBB1", "SECCFGR0") == 0xffff'ffff);
  CHECK(r.peek("GTZC1_TZIC", "SR4") == own);
  CHECK(r.read("GTZC1_MPCBB1", "CR", nonsecure_priv) == 0);
  r.write("GTZC1_TZIC", "FCR4", own, secure_priv);

  r.write("GTZC1_MPCBB1", "PRIVCFGR0", 0xffff'fffe, secure_priv);
  r.write("GTZC1_MPCBB1", "SECCFGR0", 0xffff'fffa, secure_unpriv);
  CHECK(r.peek("GTZC1_MPCBB1", "SECCFGR0") == 0xffff'fffe);
  r.write("GTZC1_MPCBB1", "SECCFGR0", 0xffff'fffa, secure_priv);
  CHECK(r.peek("GTZC1_MPCBB1", "SECCFGR0") == 0xffff'fffa);
  r.write("GTZC1_MPCBB1", "PRIVCFGR0", 0, nonsecure_priv);
  CHECK(r.peek("GTZC1_MPCBB1", "PRIVCFGR0") == 0xffff'fffa);
  r.write("GTZC1_MPCBB1", "PRIVCFGR0", 0, secure_unpriv);
  CHECK(r.peek("GTZC1_MPCBB1", "PRIVCFGR0") == 0xffff'fffa);
  CHECK(r.peek("GTZC1_TZIC", "SR4") == 0);

  CHECK(blocks->block_secure(0x2000'0000) == false);
  CHECK(blocks->block_secure(0x2000'0200) == true);
  CHECK(blocks->block_secure(0x2000'0400) == false);
  CHECK(blocks->block_secure(0x2000'05ff) == false);
  CHECK(blocks->block_secure(0x2000'0600) == true);
  CHECK(blocks->block_privileged(0x2000'0000) == false);
  CHECK(blocks->block_privileged(0x2000'0200) == true);
  CHECK_FALSE(blocks->block_secure(0x2003'0000).has_value());
  CHECK_FALSE(blocks->secure_data_allowed());
  r.write("GTZC1_MPCBB1", "CR", bit("GTZC1_MPCBB1", "CR", "SRWILADIS"), secure_priv);
  CHECK(blocks->secure_data_allowed());
  r.write("GTZC1_MPCBB1", "CR", bit("GTZC1_MPCBB1", "CR", "GLOCK"), secure_priv);
  r.write("GTZC1_MPCBB1", "CR", bit("GTZC1_MPCBB1", "CR", "SRWILADIS"), secure_priv);
  CHECK(r.peek("GTZC1_MPCBB1", "CR") == bit("GTZC1_MPCBB1", "CR", "GLOCK"));

  auto const* sram4 = dynamic_cast<stm32::sram_block_controller*>(r.target->model("GTZC2_MPCBB4"));
  REQUIRE(sram4 != nullptr);
  r.write("GTZC2_MPCBB4", "SECCFGR0", 0x7fff'ffff, secure_priv);
  CHECK(sram4->block_secure(0x2800'3e00) == false);
  CHECK(sram4->block_secure(0x2800'3dff) == true);
  CHECK_FALSE(sram4->block_secure(0x2800'4000).has_value());
}

TEST_CASE("without TrustZone no GTZC block has a model and every register stays storage")
{
  rig const r{false};
  CHECK(r.target->model("GTZC1_TZSC") == nullptr);
  CHECK(r.target->model("GTZC2_TZSC") == nullptr);
  CHECK(r.target->model("GTZC1_TZIC") == nullptr);
  CHECK(r.target->model("GTZC2_TZIC") == nullptr);
  CHECK(r.target->model("GTZC1_MPCBB1") == nullptr);
  CHECK(r.target->model("GTZC1_MPCBB2") == nullptr);
  CHECK(r.target->model("GTZC1_MPCBB3") == nullptr);
  CHECK(r.target->model("GTZC2_MPCBB4") == nullptr);
  r.write("GTZC1_TZSC", "SECCFGR1", bit("GTZC1_TZSC", "SECCFGR1", "TIM2SEC"), nonsecure_unpriv);
  CHECK(r.peek("GTZC1_TZSC", "SECCFGR1") == bit("GTZC1_TZSC", "SECCFGR1", "TIM2SEC"));
  r.write("TIM2", "CR1", 0x1, nonsecure_unpriv);
  CHECK(r.peek("TIM2", "CR1") == 0x1);
  r.write("GTZC1_TZIC", "IER1", 0x3, nonsecure_unpriv);
  CHECK(r.peek("GTZC1_TZIC", "IER1") == 0x3);
}

TEST_CASE("a snapshot keeps the TZIC flags, the TZSC bits and the GTZC line")
{
  rig const r{true};
  r.write("GTZC1_TZSC", "SECCFGR1", bit("GTZC1_TZSC", "SECCFGR1", "TIM2SEC"), secure_priv);
  r.write("GTZC1_TZIC", "IER1", bit("GTZC1_TZIC", "IER1", "TIM2IE"), secure_priv);
  r.write("TIM2", "CR1", 0x1, nonsecure_priv);
  auto const saved = r.target->snapshot();
  r.write("GTZC1_TZIC", "FCR1", bit("GTZC1_TZIC", "FCR1", "CTIM2F"), secure_priv);
  r.write("GTZC1_TZSC", "SECCFGR1", 0, secure_priv);
  REQUIRE(r.peek("GTZC1_TZIC", "SR1") == 0);
  r.target->restore(saved);
  CHECK(r.peek("GTZC1_TZIC", "SR1") == bit("GTZC1_TZIC", "SR1", "TIM2F"));
  CHECK(r.irq_pending(gtzc_irq));
  r.write("TIM2", "CR1", 0x1, nonsecure_priv);
  CHECK(r.peek("TIM2", "CR1") == 0);
  r.write("GTZC1_TZIC", "FCR1", bit("GTZC1_TZIC", "FCR1", "CTIM2F"), secure_priv);
  CHECK(r.peek("GTZC1_TZIC", "SR1") == 0);
  r.write("TIM2", "CR1", 0x1, nonsecure_priv);
  CHECK(r.peek("GTZC1_TZIC", "SR1") == bit("GTZC1_TZIC", "SR1", "TIM2F"));
}

constexpr address code_at = 0x2000'0000;

struct cpu_access
{
  std::uint16_t insn{};
  bool          nonsecure{};
  bool          unprivileged{};
  address       code{code_at};
};

constexpr address       sau_ctrl  = 0xe000'edd0;
constexpr std::uint32_t sau_allns = 2;
constexpr std::uint16_t str_r1_r0 = 0x6001;
constexpr std::uint16_t ldr_r2_r0 = 0x6802;
constexpr std::uint16_t bkpt      = 0xbe01;

auto set(rig const& r, std::string_view name, std::uint64_t value) -> void
{
  r.target->regs().write(*r.target->regs().find(name), value);
}

auto get(rig const& r, std::string_view name) -> std::uint64_t
{
  return r.target->regs().read(*r.target->regs().find(name));
}

auto engine_secure(rig const& r, bool secure) -> void
{
  std::uint32_t value = secure ? 1 : 0;
  REQUIRE(
      uc_reg_write(core::machine_internals::engine(*r.target), UC_ARM_REG_SECURE, &value)
      == UC_ERR_OK);
}

constexpr address sram1     = 0x2000'0000;
constexpr address block_512 = 0x200;

auto block_bits(rig const& r, address at, bool secure, bool privileged) -> void
{
  auto const block = (at - sram1) / block_512;
  auto const mask  = std::uint32_t{1} << (block % 32);
  auto const sec   = std::format("SECCFGR{}", block / 32);
  auto const priv  = std::format("PRIVCFGR{}", block / 32);
  auto const was   = r.peek("GTZC1_MPCBB1", sec);
  r.write("GTZC1_MPCBB1", sec, secure ? was | mask : was & ~mask, secure_priv);
  auto const was_priv = r.peek("GTZC1_MPCBB1", priv);
  r.write("GTZC1_MPCBB1", priv, privileged ? was_priv | mask : was_priv & ~mask, secure_priv);
}

auto enter_state(rig const& r, bool nonsecure) -> void
{
  REQUIRE(r.target->mem().write(sau_ctrl, 4, nonsecure ? sau_allns : 0));
  engine_secure(r, not nonsecure);
}

auto run_cpu(rig const& r, cpu_access const& access, address at, std::uint32_t value) -> void
{
  block_bits(r, access.code, not access.nonsecure, not access.unprivileged);
  if(access.nonsecure)
  {
    enter_state(r, true);
  }
  set(r, "control", access.unprivileged ? 1 : 0);
  REQUIRE(r.target->mem().write(access.code, 2, access.insn));
  REQUIRE(r.target->mem().write(access.code + 2, 2, bkpt));
  set(r, "r0", at);
  set(r, "r1", value);
  set(r, "r2", 0xdead'beef);
  set(r, "pc", access.code | 1u);
  auto const result = r.target->run_slice({.max_instructions = icount{2}});
  REQUIRE(result.stopped.has_value());
  CHECK(result.stopped->reason == popcorn::stop_reason::user);
  CHECK(get(r, "pc") == access.code + 4);
  set(r, "control", 0);
  if(access.nonsecure)
  {
    enter_state(r, false);
  }
}

TEST_CASE("RM0456 5.4.2: CPU accesses to a Secure and privileged TIM2 from each state")
{
  rig const  r{true};
  auto const cr1 = address_of("TIM2", "CR1");
  r.write("TIM2", "CR1", 0x80, secure_priv);
  r.write("GTZC1_TZSC", "SECCFGR1", bit("GTZC1_TZSC", "SECCFGR1", "TIM2SEC"), secure_priv);

  run_cpu(r, {.insn = str_r1_r0, .nonsecure = true}, cr1, 0x1);
  CHECK(r.peek("TIM2", "CR1") == 0x80);
  CHECK(r.peek("GTZC1_TZIC", "SR1") == bit("GTZC1_TZIC", "SR1", "TIM2F"));
  run_cpu(r, {.insn = ldr_r2_r0, .nonsecure = true}, cr1, 0);
  CHECK(get(r, "r2") == 0);
  r.write("GTZC1_TZIC", "FCR1", bit("GTZC1_TZIC", "FCR1", "CTIM2F"), secure_priv);

  run_cpu(r, {.insn = ldr_r2_r0}, cr1, 0);
  CHECK(get(r, "r2") == 0x80);
  run_cpu(r, {.insn = str_r1_r0}, cr1, 0x81);
  CHECK(r.peek("TIM2", "CR1") == 0x81);

  r.write("GTZC1_TZSC", "PRIVCFGR1", bit("GTZC1_TZSC", "PRIVCFGR1", "TIM2PRIV"), secure_priv);
  run_cpu(r, {.insn = str_r1_r0, .unprivileged = true}, cr1, 0x80);
  CHECK(r.peek("TIM2", "CR1") == 0x81);
  run_cpu(r, {.insn = ldr_r2_r0, .unprivileged = true}, cr1, 0);
  CHECK(get(r, "r2") == 0);
  CHECK(r.peek("GTZC1_TZIC", "SR1") == 0);
}

auto nonsecure_load(rig const& r, address at, bool sau_over, std::uint16_t insn = ldr_r2_r0)
    -> popcorn::stop
{
  constexpr address code    = 0x2000'2000;
  constexpr address sau_rnr = 0xe000'edd8;
  auto const        region  = [&r](std::uint32_t number, address base, address limit) {
    REQUIRE(r.target->mem().write(sau_rnr, 4, number));
    REQUIRE(r.target->mem().write(sau_rnr + 4, 4, base));
    REQUIRE(r.target->mem().write(sau_rnr + 8, 4, (limit & ~0x1fu) | 1u));
  };
  block_bits(r, code, false, true);
  region(0, code, code + 0x1e0);
  if(sau_over)
  {
    region(1, at & ~0x1fu, at & ~0x1fu);
  }
  REQUIRE(r.target->mem().write(sau_ctrl, 4, 1));
  REQUIRE(r.target->mem().write(code, 2, insn));
  REQUIRE(r.target->mem().write(code + 2, 2, bkpt));
  engine_secure(r, false);
  set(r, "r0", at);
  set(r, "r2", 0xdead'beef);
  set(r, "pc", code | 1u);
  auto const result = r.target->run_slice({.max_instructions = icount{2}});
  REQUIRE(result.stopped.has_value());
  return *result.stopped;
}

TEST_CASE(
    "the U5 IDAU makes 0xE0000000-0xFFFFFFFF Non-secure and exempts the external PPB, RM0456 "
    "Figures 3 to 6 and B10.4 RMVCM")
{
  constexpr address dbgmcu_idcode = 0xe004'4000;
  constexpr address past_ppb      = 0xe010'0000;
  {
    rig const  r{true};
    auto const id = r.target->mem().read(dbgmcu_idcode, 4);
    REQUIRE(id.has_value());
    auto const exempt = nonsecure_load(r, dbgmcu_idcode, false);
    CHECK(exempt.reason == popcorn::stop_reason::user);
    CHECK(get(r, "r2") == *id);
  }
  {
    rig const  r{true};
    auto const opened = nonsecure_load(r, past_ppb, true);
    REQUIRE(opened.reason == popcorn::stop_reason::fault);
    CHECK(opened.fault->kind == popcorn::fault_kind::bus);
  }
  {
    rig const  r{true};
    auto const closed = nonsecure_load(r, past_ppb, false);
    REQUIRE(closed.reason == popcorn::stop_reason::fault);
    CHECK(closed.fault->kind == popcorn::fault_kind::secure);
  }
}

auto debug_warnings(std::vector<core::log_record> const& logged, std::string_view block)
    -> std::vector<core::log_record>
{
  return logged
       | std::views::filter([&](core::log_record const& record) {
           return record.kind == core::log_kind::model_warning and record.peripheral == block;
         })
       | std::ranges::to<std::vector>();
}

TEST_CASE("RM0456 75.5: the U5 debug blocks without a model are RAZ/WI with one warning, TZEN=0")
{
  rig const                     r{false};
  std::vector<core::log_record> logged;
  auto const                    sink =
      r.target->add_log_sink([&](core::log_record const& record) { logged.push_back(record); });
  auto const blocks = stm32::raz_wi_map(u575());
  REQUIRE(
      (blocks
       | std::views::transform(&popcorn::address_range::name)
       | std::ranges::to<std::vector>())
      == std::vector<std::string>{"TPIU", "ETM", "CTI", "MCU_ROM_TABLE", "CPU_ROM_TABLE"});
  for(auto const& block : blocks)
  {
    CAPTURE(block.name);
    CHECK(block.size == 0x1000);
    auto const at = block.start + 4;
    run_cpu(r, {.insn = str_r1_r0}, at, 0x1234'5678);
    run_cpu(r, {.insn = ldr_r2_r0}, at, 0);
    CHECK(get(r, "r2") == 0);
    run_cpu(r, {.insn = ldr_r2_r0}, block.start + block.size - 4, 0);
    CHECK(get(r, "r2") == 0);
    auto const warned = debug_warnings(logged, block.name);
    REQUIRE(warned.size() == 1);
    CHECK(warned.front().write);
    CHECK(warned.front().offset == 4);
  }
  constexpr address dbgmcu_cr = 0xe004'4004;
  run_cpu(r, {.insn = str_r1_r0}, dbgmcu_cr, 0x6);
  CHECK(r.target->mem().read(dbgmcu_cr, 4) == 0x6u);
}

TEST_CASE("without the RAZ/WI ranges a CTI load is a precise BusFault, TZEN=0")
{
  rig const r{false, {}, popcorn::arch::fault_policy::stop, false};
  set(r, "r0", 0xe004'2004);
  REQUIRE(r.target->mem().write(code_at, 2, ldr_r2_r0));
  REQUIRE(r.target->mem().write(code_at + 2, 2, bkpt));
  set(r, "pc", code_at | 1u);
  auto const result = r.target->run_slice({.max_instructions = icount{2}});
  REQUIRE(result.stopped.has_value());
  REQUIRE(result.stopped->reason == popcorn::stop_reason::fault);
  CHECK(result.stopped->fault->kind == popcorn::fault_kind::bus);
  CHECK(result.stopped->fault->status == 0x8200);
}

TEST_CASE("a Non-secure load and store of the CTI with no SAU region over it are RAZ/WI, TZEN=1")
{
  constexpr address cti = 0xe004'2004;
  {
    rig const                     r{true};
    std::vector<core::log_record> logged;
    auto const                    sink =
        r.target->add_log_sink([&](core::log_record const& record) { logged.push_back(record); });
    auto const loaded = nonsecure_load(r, cti, false);
    CHECK(loaded.reason == popcorn::stop_reason::user);
    CHECK(get(r, "r2") == 0);
    auto const stored = nonsecure_load(r, cti, false, str_r1_r0);
    CHECK(stored.reason == popcorn::stop_reason::user);
    auto const warned = debug_warnings(logged, "CTI");
    REQUIRE(warned.size() == 1);
    CHECK_FALSE(warned.front().write);
  }
  {
    rig const  r{true, {.tzen = true}, popcorn::arch::fault_policy::stop, false};
    auto const faulted = nonsecure_load(r, cti, false);
    REQUIRE(faulted.reason == popcorn::stop_reason::fault);
    CHECK(faulted.fault->kind == popcorn::fault_kind::bus);
  }
}

TEST_CASE("under strict_peripherals a CPU or DMA access of a debug block is a model_error")
{
  constexpr address             cti = 0xe004'2004;
  rig const                     r{false, {}, popcorn::arch::fault_policy::stop, true, true};
  std::vector<core::log_record> logged;
  auto const                    sink =
      r.target->add_log_sink([&](core::log_record const& record) { logged.push_back(record); });
  auto const cpu = [&](std::uint16_t insn) {
    REQUIRE(r.target->mem().write(code_at, 2, insn));
    REQUIRE(r.target->mem().write(code_at + 2, 2, bkpt));
    set(r, "r0", cti);
    set(r, "pc", code_at | 1u);
    try
    {
      std::ignore = r.target->run_slice({.max_instructions = icount{2}});
      FAIL("no model_error");
    }
    catch(popcorn::model_error const& error)
    {
      CHECK(error.peripheral() == "CTI");
      CHECK(error.reg().empty());
      CHECK(error.offset() == 4);
    }
  };
  cpu(ldr_r2_r0);
  cpu(str_r1_r0);
  auto& bus = r.bus->services();
  CHECK_THROWS_AS(
      std::ignore = bus.read(cti, 4, bus_attrs{.by = initiator::dma}), popcorn::model_error);
  CHECK_THROWS_AS(
      std::ignore = bus.write(cti, 4, 1, bus_attrs{.by = initiator::dma}), popcorn::model_error);
  CHECK(r.target->mem().read(0xe004'0004, 4) == 0u);
  CHECK(r.target->mem().write(0xe004'0004, 4, 1).has_value());
  CHECK(debug_warnings(logged, "CTI").size() == 1);
  CHECK(debug_warnings(logged, "TPIU").empty());
}

TEST_CASE(
    "machine_options::raz_wi refuses a range over a region, a peripheral or the scratch region, "
    "unaligned or overlapping")
{
  popcorn::address_range const arena{.name = "arena", .start = 0xc000'0000, .size = 0x1000};
  auto const refused = [&](std::vector<popcorn::address_range> ranges, std::string_view key) {
    auto options    = popcorn::testing::stm32_options();
    options.scratch = arena;
    options.raz_wi  = std::move(ranges);
    CAPTURE(key);
    try
    {
      core::machine{u575(), popcorn::arch::armv8m::factory(), std::move(options)};
      FAIL("accepted");
    }
    catch(popcorn::config_error const& error)
    {
      CHECK(error.key() == key);
    }
  };
  refused(
      {
          {.name = "SRAM", .start = 0x2000'0000, .size = 0x1000}
  },
      "raz_wi[0]");
  refused({arena}, "raz_wi[0]");
  refused(
      {
          {.name = "OVER", .start = 0xe004'4000, .size = 0x1000}
  },
      "raz_wi[0]");
  refused(
      {
          {.name = "ODD", .start = 0xe004'0200, .size = 0x1000}
  },
      "raz_wi[0]");
  refused(
      {
          {.name = "EMPTY", .start = 0xe004'0000, .size = 0}
  },
      "raz_wi[0]");
  refused(
      {
          {.name = "A", .start = 0xe004'0000, .size = 0x2000},
          {.name = "B", .start = 0xe004'1000, .size = 0x1000}
  },
      "raz_wi[1]");
}

TEST_CASE("every peripheral with a Secure alias has a SEC rule, except the TrustZone-aware ones")
{
  std::set<std::size_t> ruled;
  for(auto const& f : stm32::gtzc_fields(u575()))
  {
    if(f.owner.ends_with("_TZSC"))
    {
      ruled.insert(f.units.begin(), f.units.end());
    }
  }
  std::set<std::string> unruled;
  for(auto const& [index, p] : std::views::enumerate(u575().peripherals))
  {
    auto const aliased =
        std::ranges::any_of(u575().peripherals, [&](popcorn::peripheral_desc const& a) {
          return a.alias_of == p.name;
        });
    if(aliased and not p.alias_of and not ruled.contains(static_cast<std::size_t>(index)))
    {
      unruled.insert(p.name);
    }
  }
  CHECK(
      unruled == std::set<std::string>{"EXTI",         "FLASH",        "GPDMA1",       "GPIOA",
                                       "GPIOB",        "GPIOC",        "GPIOD",        "GPIOE",
                                       "GPIOF",        "GPIOG",        "GPIOH",        "GPIOI",
                                       "GTZC1_MPCBB1", "GTZC1_MPCBB2", "GTZC1_MPCBB3", "GTZC1_TZIC",
                                       "GTZC1_TZSC",   "GTZC2_MPCBB4", "GTZC2_TZIC",   "GTZC2_TZSC",
                                       "LPDMA1",       "LPGPIO1",      "OTFDEC1",      "OTFDEC2",
                                       "PWR",          "RCC",          "RTC",          "SYSCFG",
                                       "TAMP"});
}

TEST_CASE("RM0456 5.6.4 and the layout: one SEC bit secures each block it covers, with its event")
{
  struct tie
  {
    std::string_view sec_reg;
    std::string_view sec;
    std::string_view peripheral;
    std::string_view reg;
    std::uint32_t    value;
    std::string_view flag_reg;
    std::string_view flag;
  };
  for(auto const& one : std::initializer_list<tie>{
          {"SECCFGR3", "DCMISEC",         "PSSI",       "CR",   0x4000'0001, "SR3", "DCMIF"    },
          {"SECCFGR3", "ADC1SEC",         "ADC12",      "CCR",  0x0040'0000, "SR3", "ADC1F"    },
          {"SECCFGR1", "FDCAN1SEC",       "FDCAN1_RAM", "DBTP", 0x0000'0a34, "SR1", "FDCAN1F"  },
          {"SECCFGR3", "SDMMC1SEC",       "DLYBSD1",    "CR",   0x1,         "SR3", "SDMMC1F"  },
          {"SECCFGR3", "SDMMC2SEC",       "DLYBSD2",    "CR",   0x1,         "SR3", "SDMMC2F"  },
          {"SECCFGR3", "OCTOSPI1_REGSEC", "DLYBOS1",    "CR",   0x1,         "SR3", "OCTOSPI1F"},
          {"SECCFGR3", "OCTOSPI2_REGSEC", "DLYBOS2",    "CR",   0x1,         "SR3", "OCTOSPI2F"},
  })
  {
    std::string const name{one.peripheral};
    CAPTURE(name);
    rig const  r{true};
    auto const before = r.peek(one.peripheral, one.reg);
    r.write("GTZC1_TZSC", one.sec_reg, bit("GTZC1_TZSC", one.sec_reg, one.sec), secure_priv);
    r.write(one.peripheral, one.reg, one.value, nonsecure_priv);
    CHECK(r.peek(one.peripheral, one.reg) == before);
    CHECK(r.read(one.peripheral, one.reg, nonsecure_priv) == 0);
    CHECK(r.peek("GTZC1_TZIC", one.flag_reg) == bit("GTZC1_TZIC", one.flag_reg, one.flag));
    r.write(one.peripheral, one.reg, one.value, secure_priv);
    CHECK(r.peek(one.peripheral, one.reg) != before);
  }
}

TEST_CASE("RM0456 3.5.5: a Non-secure read alone raises the event of a Secure register")
{
  struct leg
  {
    std::string_view peripheral;
    std::string_view reg;
    std::string_view flag_reg;
    std::string_view flag;
  };
  for(auto const& one : std::initializer_list<leg>{
          {"TIM2",         "CR1",       "SR1", "TIM2F"      },
          {"GTZC1_TZSC",   "CR",        "SR4", "TZSC1F"     },
          {"GTZC1_MPCBB1", "CR",        "SR4", "MPCBB1_REGF"},
          {"GTZC1_MPCBB1", "CFGLOCKR1", "SR4", "MPCBB1_REGF"},
          {"GTZC1_TZIC",   "IER1",      "SR4", "TZIC1F"     },
  })
  {
    std::string const name = std::string{one.peripheral} + "." + std::string{one.reg};
    CAPTURE(name);
    rig const r{true};
    r.write("GTZC1_TZSC", "SECCFGR1", bit("GTZC1_TZSC", "SECCFGR1", "TIM2SEC"), secure_priv);
    REQUIRE(r.peek("GTZC1_TZIC", "SR1") == 0);
    REQUIRE(r.peek("GTZC1_TZIC", "SR4") == 0);
    CHECK(r.read(one.peripheral, one.reg, nonsecure_priv) == 0);
    CHECK(r.peek("GTZC1_TZIC", one.flag_reg) == bit("GTZC1_TZIC", one.flag_reg, one.flag));
  }
  rig const r{true};
  CHECK(r.read("GTZC1_TZSC", "SECCFGR1", nonsecure_unpriv) == 0);
  CHECK(r.read("GTZC1_MPCBB1", "SECCFGR0", nonsecure_unpriv) == 0xffff'ffff);
  CHECK(r.read("GTZC1_TZSC", "PRIVCFGR1", nonsecure_unpriv) == 0);
  CHECK(r.peek("GTZC1_TZIC", "SR4") == 0);
  CHECK(r.peek("GTZC1_TZIC", "SR1") == 0);
}

TEST_CASE("GTZC_IRQn is the OR of both TZICs and drops only when the last enabled flag clears")
{
  constexpr address icpr0 = 0xe000'e280;
  rig const         r{true};
  auto const        tim2    = bit("GTZC1_TZIC", "SR1", "TIM2F");
  auto const        lpuart1 = bit("GTZC2_TZIC", "SR1", "LPUART1F");
  auto const clear8 = [&] { REQUIRE(r.target->mem().write(icpr0, 4, 1u << gtzc_irq).has_value()); };
  r.write("GTZC1_TZSC", "SECCFGR1", bit("GTZC1_TZSC", "SECCFGR1", "TIM2SEC"), secure_priv);
  r.write("GTZC2_TZSC", "SECCFGR1", bit("GTZC2_TZSC", "SECCFGR1", "LPUART1SEC"), secure_priv);
  r.write("GTZC1_TZIC", "IER1", bit("GTZC1_TZIC", "IER1", "TIM2IE"), secure_priv);
  r.write("GTZC2_TZIC", "IER1", bit("GTZC2_TZIC", "IER1", "LPUART1IE"), secure_priv);
  r.write("TIM2", "CR1", 0x1, nonsecure_priv);
  r.write("LPUART1", "CR1", 0x1, nonsecure_priv);
  REQUIRE(r.peek("GTZC1_TZIC", "SR1") == tim2);
  REQUIRE(r.peek("GTZC2_TZIC", "SR1") == lpuart1);
  CHECK(r.irq_pending(gtzc_irq));

  r.write("GTZC2_TZIC", "FCR1", lpuart1, secure_priv);
  clear8();
  CHECK(r.irq_pending(gtzc_irq));
  r.write("GTZC1_TZIC", "FCR1", tim2, secure_priv);
  clear8();
  CHECK_FALSE(r.irq_pending(gtzc_irq));

  r.write("LPUART1", "CR1", 0x1, nonsecure_priv);
  CHECK(r.irq_pending(gtzc_irq));
  r.write("TIM2", "CR1", 0x1, nonsecure_priv);
  r.write("GTZC1_TZIC", "FCR1", tim2, secure_priv);
  clear8();
  CHECK(r.irq_pending(gtzc_irq));
  r.write("GTZC2_TZIC", "FCR1", lpuart1, secure_priv);
  clear8();
  CHECK_FALSE(r.irq_pending(gtzc_irq));
}

constexpr address       block_1      = sram1 + block_512;
constexpr address       block_2      = sram1 + 2 * block_512;
constexpr address       block_3      = sram1 + 3 * block_512;
constexpr address       loop_at      = 0x2000'2000;
constexpr address       vectors_at   = 0x2000'3000;
constexpr address       handler_at   = 0x2000'3100;
constexpr address       stack_top    = 0x2000'3800;
constexpr address       vtor         = 0xe000'ed08;
constexpr address       cfsr         = 0xe000'ed28;
constexpr address       hfsr         = 0xe000'ed2c;
constexpr address       sau_rnr      = 0xe000'edd8;
constexpr address       sau_rbar     = 0xe000'eddc;
constexpr address       sau_rlar     = 0xe000'ede0;
constexpr address       flash_ns     = 0x0800'0000;
constexpr address       flash_s      = 0x0c00'0000;
constexpr address       flash_page   = 0x2000;
constexpr address       backup_ns    = 0x4003'6400;
constexpr address       backup_s     = 0x5003'6400;
constexpr std::uint32_t cfsr_ibuserr = 1u << 8;
constexpr std::uint32_t hfsr_forced  = 1u << 30;
constexpr std::uint16_t adds_r3_1    = 0x3301;
constexpr std::uint16_t nop          = 0xbf00;
constexpr std::uint16_t bx_lr        = 0x4770;

auto sram_flag(rig const& r) -> bool
{
  return (r.peek("GTZC1_TZIC", "SR4") & bit("GTZC1_TZIC", "SR4", "SRAM1F")) != 0;
}

auto clear_events(rig const& r) -> void
{
  r.write("GTZC1_TZIC", "FCR4", ~0u, secure_priv);
  REQUIRE(r.peek("GTZC1_TZIC", "SR4") == 0);
}

auto put_code(rig const& r, address at, std::initializer_list<std::uint16_t> code) -> void
{
  for(auto const& [index, insn] : std::views::enumerate(code))
  {
    REQUIRE(r.target->mem().write(at + 2 * static_cast<address>(index), 2, insn));
  }
}

auto word(rig const& r, address at) -> std::uint32_t
{
  auto const value = r.target->mem().read(at, 4);
  REQUIRE(value.has_value());
  return *value;
}

struct fetched
{
  popcorn::stop stopped;
  icount        executed{};
};

auto run_from(rig const& r, address pc, icount bound, icount per_slice) -> fetched
{
  set(r, "pc", pc | 1u);
  fetched out;
  while(out.executed < bound)
  {
    auto const result = r.target->run_slice(
        {.max_instructions = std::min(per_slice, icount{bound.value - out.executed.value}),
         .wall_budget      = std::chrono::seconds{60}});
    out.executed = icount{out.executed.value + result.executed.value};
    if(result.stopped)
    {
      out.stopped = *result.stopped;
      return out;
    }
  }
  FAIL("no stop within the bound");
  return out;
}

auto expect_fetch_fault(fetched const& run) -> void
{
  REQUIRE(run.stopped.reason == popcorn::stop_reason::fault);
  REQUIRE(run.stopped.fault.has_value());
  CHECK(run.stopped.fault->kind == popcorn::fault_kind::bus);
  CHECK((run.stopped.fault->status & cfsr_ibuserr) != 0);
}

TEST_CASE("RM0456 5.4.2: MPCBB blocks filter the CPU data accesses of each state")
{
  rig const r{true};
  block_bits(r, block_2, false, true);
  REQUIRE(r.target->mem().write(block_1, 4, 0xcafe'f00d));

  run_cpu(r, {.insn = str_r1_r0, .nonsecure = true, .code = loop_at}, block_2, 0x1234);
  CHECK(word(r, block_2) == 0x1234);
  run_cpu(r, {.insn = ldr_r2_r0, .nonsecure = true, .code = loop_at}, block_2, 0);
  CHECK(get(r, "r2") == 0x1234);
  CHECK_FALSE(sram_flag(r));

  run_cpu(r, {.insn = ldr_r2_r0, .nonsecure = true, .code = loop_at}, block_1, 0);
  CHECK(get(r, "r2") == 0);
  CHECK(sram_flag(r));
  clear_events(r);
  run_cpu(r, {.insn = str_r1_r0, .nonsecure = true, .code = loop_at}, block_1, 0x55);
  CHECK(word(r, block_1) == 0xcafe'f00d);
  CHECK(sram_flag(r));
  clear_events(r);

  run_cpu(r, {.insn = ldr_r2_r0, .code = loop_at}, block_2, 0);
  CHECK(get(r, "r2") == 0);
  CHECK(sram_flag(r));
  clear_events(r);
  run_cpu(r, {.insn = ldr_r2_r0, .code = loop_at}, block_1, 0);
  CHECK(get(r, "r2") == 0xcafe'f00d);
  CHECK_FALSE(sram_flag(r));

  r.write("GTZC1_MPCBB1", "CR", bit("GTZC1_MPCBB1", "CR", "SRWILADIS"), secure_priv);
  run_cpu(r, {.insn = ldr_r2_r0, .code = loop_at}, block_2, 0);
  CHECK(get(r, "r2") == 0x1234);
  run_cpu(r, {.insn = str_r1_r0, .code = loop_at}, block_2, 0x5678);
  CHECK(word(r, block_2) == 0x5678);
  CHECK_FALSE(sram_flag(r));

  block_bits(r, block_1, true, true);
  run_cpu(r, {.insn = ldr_r2_r0, .unprivileged = true, .code = loop_at}, block_1, 0);
  CHECK(get(r, "r2") == 0);
  CHECK_FALSE(sram_flag(r));
  block_bits(r, block_1, true, false);
  run_cpu(r, {.insn = ldr_r2_r0, .unprivileged = true, .code = loop_at}, block_1, 0);
  CHECK(get(r, "r2") == 0xcafe'f00d);
}

TEST_CASE("RM0456 5.4.2: a fetch the MPCBB refuses is a BusFault, for each state")
{
  rig const r{true};
  block_bits(r, block_1, true, true);
  put_code(r, block_1, {nop, bkpt});
  enter_state(r, true);
  auto const nonsecure = run_from(r, block_1, icount{4}, icount{4});
  expect_fetch_fault(nonsecure);
  CHECK(get(r, "pc") == block_1);
  CHECK_FALSE(sram_flag(r));
  enter_state(r, false);

  rig const handled{true, {.tzen = true}, popcorn::arch::fault_policy::enter_handler};
  block_bits(handled, block_1, false, true);
  put_code(handled, block_1, {nop, bkpt});
  put_code(handled, handler_at, {bkpt});
  REQUIRE(handled.target->mem().write(vectors_at, 4, stack_top));
  REQUIRE(handled.target->mem().write(vectors_at + 4 * 3, 4, handler_at | 1u));
  REQUIRE(handled.target->mem().write(vtor, 4, vectors_at));
  set(handled, "msp", stack_top);
  auto const secure = run_from(handled, block_1, icount{8}, icount{8});
  CHECK(secure.stopped.reason == popcorn::stop_reason::user);
  CHECK(get(handled, "pc") == handler_at + 2);
  CHECK((get(handled, "xpsr") & 0x1ff) == 3);
  CHECK((word(handled, hfsr) & hfsr_forced) != 0);
  CHECK((word(handled, cfsr) & cfsr_ibuserr) != 0);
  CHECK(sram_flag(handled));
}

TEST_CASE("a 1 KB page whose two 512-byte blocks differ checks every fetch, both directions")
{
  rig const r{true};
  block_bits(r, block_2, false, true);
  put_code(
      r, block_2 + block_512 - 16,
      {adds_r3_1, adds_r3_1, adds_r3_1, adds_r3_1, adds_r3_1, adds_r3_1, adds_r3_1, adds_r3_1});
  put_code(r, block_3, {adds_r3_1, bkpt});
  set(r, "r3", 0);
  enter_state(r, true);
  auto const into_secure = run_from(r, block_2 + block_512 - 16, icount{32}, icount{32});
  expect_fetch_fault(into_secure);
  CHECK(get(r, "pc") == block_3);
  CHECK(get(r, "r3") == 8);
  enter_state(r, false);

  rig const back{true};
  block_bits(back, block_3, false, true);
  put_code(back, block_3, {adds_r3_1, 0xe6fd});
  put_code(back, block_2, {adds_r3_1, bkpt});
  set(back, "r3", 0);
  enter_state(back, true);
  auto const into_lower = run_from(back, block_3, icount{8}, icount{8});
  expect_fetch_fault(into_lower);
  CHECK(get(back, "pc") == block_2);
  CHECK(get(back, "r3") == 1);
  enter_state(back, false);
}

auto chained_flip(icount per_slice) -> std::tuple<fetched, std::uint64_t, std::uint64_t, bool>
{
  rig const  r{true};
  auto const seccfgr0 = address_of("GTZC1_MPCBB1", "SECCFGR0");
  auto const opened   = ~(std::uint32_t{1} << ((loop_at - sram1) / block_512));
  put_code(r, loop_at, {0x3b01, 0x42a3, 0xbf08, 0x4631, str_r1_r0, 0x2b00, 0xd1f8, bkpt});
  set(r, "r0", seccfgr0);
  set(r, "r1", 0xffff'ffff);
  set(r, "r6", opened);
  set(r, "r3", 100'000);
  set(r, "r4", 99'000);
  auto const run = run_from(r, loop_at, icount{800'000}, per_slice);
  return {run, get(r, "pc"), get(r, "r3"), sram_flag(r)};
}

TEST_CASE("a fetch-verdict change from a store in a chained loop faults at the next instruction")
{
  auto const [whole, pc, r3, flagged] = chained_flip(icount{800'000});
  expect_fetch_fault(whole);
  CHECK(pc == loop_at + 10);
  CHECK(r3 == 99'000);
  CHECK(flagged);
  CHECK(whole.executed == icount{999 * 7 + 5});
  for(auto const per_slice : {icount{1000}, icount{6997}, icount{3499}})
  {
    CAPTURE(per_slice.value);
    auto const [sliced, sliced_pc, sliced_r3, sliced_flagged] = chained_flip(per_slice);
    expect_fetch_fault(sliced);
    CHECK(sliced_pc == pc);
    CHECK(sliced_r3 == r3);
    CHECK(sliced.executed == whole.executed);
    CHECK(sliced_flagged);
  }
}

auto rewrite_loop(std::uint32_t toggle) -> std::pair<std::uint64_t, std::uint64_t>
{
  rig const  r{true};
  auto*      engine = core::machine_internals::engine(*r.target);
  auto const before = core::machine_internals::counters(*r.target).engine_runs;
  put_code(r, loop_at, {0x4071, str_r1_r0, 0x682a, 0x3b01, 0xd1fa, bkpt});
  set(r, "r0", address_of("GTZC1_MPCBB1", "SECCFGR1"));
  set(r, "r1", 0xffff'ffff);
  set(r, "r5", 0x2000'3000);
  set(r, "r6", toggle);
  set(r, "r3", 1000);
  REQUIRE(uc_ctl_slow_path_count_reset(engine) == UC_ERR_OK);
  auto const run = run_from(r, loop_at, icount{10'000}, icount{10'000});
  REQUIRE(run.stopped.reason == popcorn::stop_reason::user);
  std::uint64_t slow = 0;
  REQUIRE(uc_ctl_slow_path_count(engine, &slow) == UC_ERR_OK);
  return {slow, core::machine_internals::counters(*r.target).engine_runs - before};
}

TEST_CASE("rewriting an MPCBB SECCFGR word with its value keeps the page verdicts and the run")
{
  auto const [same_slow, same_runs]       = rewrite_loop(0);
  auto const [changed_slow, changed_runs] = rewrite_loop(1);
  CHECK(same_slow <= 2);
  CHECK(same_runs <= 2);
  CHECK(changed_slow >= 1000);
  CHECK(changed_runs >= 1000);
  MESSAGE(
      "same value: ", same_slow, " slow-path loads, ", same_runs,
      " engine runs, changed: ", changed_slow, ", ", changed_runs);
}

TEST_CASE(
    "RM0456 Table 68: flash pages filter data and fetches by the transaction's security, SECWM1 "
    "0 to 1")
{
  rig const r{
      true, {.secwm1 = stm32::page_range{.first = 0, .last = 1}, .secwm2 = stm32::no_area{}}
  };
  auto const flash_flag = [&r] {
    return (r.peek("GTZC1_TZIC", "SR4") & bit("GTZC1_TZIC", "SR4", "FLASHF")) != 0;
  };
  REQUIRE(r.target->mem().write(flash_ns, 4, 0x1111'1111));
  REQUIRE(r.target->mem().write(flash_ns + 2 * flash_page, 4, 0x2222'2222));

  run_cpu(r, {.insn = ldr_r2_r0, .nonsecure = true, .code = loop_at}, flash_ns, 0);
  CHECK(get(r, "r2") == 0);
  CHECK(flash_flag());
  clear_events(r);
  run_cpu(r, {.insn = ldr_r2_r0, .nonsecure = true, .code = loop_at}, flash_ns + 2 * flash_page, 0);
  CHECK(get(r, "r2") == 0x2222'2222);
  run_cpu(r, {.insn = ldr_r2_r0, .code = loop_at}, flash_s, 0);
  CHECK(get(r, "r2") == 0x1111'1111);
  CHECK_FALSE(flash_flag());

  run_cpu(r, {.insn = ldr_r2_r0, .code = loop_at}, flash_s + 2 * flash_page, 0);
  CHECK(get(r, "r2") == 0);
  CHECK(flash_flag());
  clear_events(r);

  REQUIRE(r.target->mem().write(sau_rnr, 4, 0));
  REQUIRE(r.target->mem().write(sau_rbar, 4, flash_ns + 2 * flash_page));
  REQUIRE(r.target->mem().write(sau_rlar, 4, (flash_ns + 3 * flash_page - 32) | 1u));
  REQUIRE(r.target->mem().write(sau_ctrl, 4, 1));
  run_cpu(r, {.insn = ldr_r2_r0, .code = loop_at}, flash_ns + 2 * flash_page, 0);
  CHECK(get(r, "r2") == 0x2222'2222);
  CHECK_FALSE(flash_flag());
  REQUIRE(r.target->mem().write(sau_ctrl, 4, 0));

  REQUIRE(r.target->mem().write(flash_ns, 2, bkpt));
  REQUIRE(r.target->mem().write(flash_ns + 2 * flash_page, 2, bkpt));
  enter_state(r, true);
  expect_fetch_fault(run_from(r, flash_ns, icount{2}, icount{2}));
  CHECK(get(r, "pc") == flash_ns);
  CHECK_FALSE(flash_flag());
  enter_state(r, false);
  expect_fetch_fault(run_from(r, flash_s + 2 * flash_page, icount{2}, icount{2}));
  CHECK(get(r, "pc") == flash_s + 2 * flash_page);
  CHECK_FALSE(flash_flag());
  auto const control = run_from(r, flash_s, icount{2}, icount{2});
  CHECK(control.stopped.reason == popcorn::stop_reason::user);
}

TEST_CASE("host accesses by DMA and stacking obey the memory policy, the debugger does not")
{
  rig const r{true};
  REQUIRE(r.target->mem().write(block_1, 4, 0xcafe'f00d));
  auto& bus = r.bus->services();
  CHECK(bus.read(block_1, 4, bus_attrs{.by = initiator::dma, .secure = false}) == 0u);
  CHECK(sram_flag(r));
  clear_events(r);
  CHECK(bus.write(block_1, 4, 0x55, bus_attrs{.by = initiator::dma, .secure = false}).has_value());
  CHECK(word(r, block_1) == 0xcafe'f00d);
  CHECK(sram_flag(r));
  clear_events(r);
  std::array<std::byte, 8> copied{};
  CHECK(bus.read_block(block_1, copied, bus_attrs{.by = initiator::dma, .secure = false})
            .has_value());
  CHECK(std::ranges::all_of(copied, [](std::byte b) { return b == std::byte{0}; }));
  CHECK(bus.read(block_1, 4, bus_attrs{.by = initiator::dma, .secure = true}) == 0xcafe'f00du);
  CHECK(
      bus.read(block_1, 4, bus_attrs{.by = initiator::debugger, .secure = false}) == 0xcafe'f00du);
  clear_events(r);

  rig const plain{false};
  REQUIRE(plain.target->mem().write(block_1, 4, 0xcafe'f00d));
  CHECK(
      plain.bus->services().read(block_1, 4, bus_attrs{.by = initiator::dma, .secure = false})
      == 0xcafe'f00du);
}

TEST_CASE("a read the MPCBB refuses reports 0 to a read watch, from the CPU and from DMA")
{
  rig const r{true};
  REQUIRE(r.target->mem().write(block_1, 4, 0xcafe'f00d));
  std::vector<core::watch_hit> hits;
  auto const                   watch =
      r.target->add_watch(block_1, 4, core::watch_kind::read, [&hits](core::watch_hit const& hit) {
        hits.push_back(hit);
        return core::watch_action::resume;
      });
  run_cpu(r, {.insn = ldr_r2_r0, .nonsecure = true, .code = loop_at}, block_1, 0);
  CHECK(get(r, "r2") == 0);
  CHECK(r.bus->services().read(block_1, 4, bus_attrs{.by = initiator::dma, .secure = false}) == 0u);
  REQUIRE(hits.size() == 2);
  CHECK(hits[0].by == initiator::cpu);
  CHECK(hits[0].value == 0);
  CHECK(hits[0].pc == loop_at);
  CHECK(hits[1].by == initiator::dma);
  CHECK(hits[1].value == 0);
  run_cpu(r, {.insn = ldr_r2_r0, .code = loop_at}, block_1, 0);
  REQUIRE(hits.size() == 3);
  CHECK(hits[2].value == 0xcafe'f00d);
}

TEST_CASE("Secure exception stacking into a Non-secure block is dropped with an event")
{
  auto const stacked = [](bool open) {
    rig const r{true, {.tzen = true}, popcorn::arch::fault_policy::enter_handler};
    if(open)
    {
      block_bits(r, block_2, false, true);
    }
    for(address at = block_2 + 0x100 - 0x40; at < block_2 + 0x100; at += 4)
    {
      REQUIRE(r.target->mem().write(at, 4, 0xa5a5'a5a5));
    }
    put_code(r, loop_at, {0xdf00, bkpt});
    put_code(r, handler_at, {bkpt});
    REQUIRE(r.target->mem().write(vectors_at, 4, stack_top));
    REQUIRE(r.target->mem().write(vectors_at + 4 * 11, 4, handler_at | 1u));
    REQUIRE(r.target->mem().write(vtor, 4, vectors_at));
    set(r, "msp", block_2 + 0x100);
    set(r, "r0", 0x1234'5678);
    auto const run = run_from(r, loop_at, icount{4}, icount{4});
    CHECK(run.stopped.reason == popcorn::stop_reason::user);
    CHECK(get(r, "pc") == handler_at + 2);
    return std::pair{word(r, block_2 + 0x100 - 0x20), sram_flag(r)};
  };
  auto const [kept, flagged] = stacked(false);
  CHECK(kept == 0x1234'5678);
  CHECK_FALSE(flagged);
  auto const [dropped, dropped_flag] = stacked(true);
  CHECK(dropped == 0xa5a5'a5a5);
  CHECK(dropped_flag);
}

TEST_CASE("a call in Non-secure state writes its stack arguments as Non-secure transactions")
{
  rig const r{true};
  block_bits(r, loop_at, false, true);
  block_bits(r, loop_at - block_512, false, true);
  put_code(r, loop_at, {0x9800, 0x4770});
  enter_state(r, true);
  popcorn::arch::call_frame frame{
      .entry           = loop_at | 1u,
      .returns         = popcorn::arch::value_kind::i32,
      .return_sentinel = loop_at + 0x100,
      .stack           = loop_at - 0x10};
  for(std::uint32_t arg = 1; arg <= 5; ++arg)
  {
    frame.args.push_back({.kind = popcorn::arch::value_kind::i32, .bits = 0x50 + arg});
  }
  auto const result = r.target->call(frame, icount{100});
  CHECK(result.stopped.reason == popcorn::stop_reason::returned);
  CHECK(result.value == 0x55);
  CHECK_FALSE(sram_flag(r));
  enter_state(r, false);
}

TEST_CASE("MPCWM4: BKPSRAM is Secure and privileged outside its enabled sub-region, RM0456 5.6.8")
{
  rig const  r{true};
  auto&      bus  = r.bus->services();
  auto const flag = [&r] {
    return (r.peek("GTZC1_TZIC", "SR4") & bit("GTZC1_TZIC", "SR4", "BKPSRAMF")) != 0;
  };
  auto const ns_read = [&](address at) {
    return bus.read(at, 4, bus_attrs{.by = initiator::dma, .secure = false});
  };
  REQUIRE(r.target->mem().write(backup_ns + 0x20, 4, 0x2020'2020));
  REQUIRE(r.target->mem().write(backup_ns + 0x5c, 4, 0x5c5c'5c5c));
  REQUIRE(r.target->mem().write(backup_ns + 0x60, 4, 0x6060'6060));
  REQUIRE(r.target->mem().write(backup_ns, 4, 0x0101'0101));
  CHECK(ns_read(backup_ns + 0x20) == 0u);
  CHECK(flag());
  clear_events(r);

  auto const config = std::string{"TZSC_MPCWM4ACFGR"};
  auto const area   = std::string{"TZSC_MPCWM4AR"};
  r.write("GTZC1_TZSC", area, 2u << 16 | 1u, secure_priv);
  r.write("GTZC1_TZSC", config, bit("GTZC1_TZSC", config, "SREN"), secure_priv);
  CHECK(ns_read(backup_ns + 0x20) == 0x2020'2020u);
  CHECK(ns_read(backup_ns + 0x5c) == 0x5c5c'5c5cu);
  CHECK(ns_read(backup_ns + 0x60) == 0u);
  CHECK(ns_read(backup_ns) == 0u);
  CHECK(flag());
  clear_events(r);
  CHECK(bus.read(backup_s + 0x20, 4, bus_attrs{.by = initiator::dma, .secure = true}) == 0u);
  CHECK(flag());
  clear_events(r);
  CHECK(bus.read(backup_s, 4, bus_attrs{.by = initiator::dma, .secure = true}) == 0x0101'0101u);
  CHECK(
      bus.read(
          backup_ns + 0x20, 4,
          bus_attrs{.by = initiator::dma, .secure = false, .privileged = false})
      == 0x2020'2020u);

  r.write("GTZC1_TZSC", config, 0, nonsecure_priv);
  CHECK(r.peek("GTZC1_TZSC", config) == bit("GTZC1_TZSC", config, "SREN"));
  CHECK((r.peek("GTZC1_TZIC", "SR4") & bit("GTZC1_TZIC", "SR4", "TZSC1F")) != 0);
  CHECK(r.read("GTZC1_TZSC", area, nonsecure_priv) == 0);
  r.write("GTZC1_TZSC", area, 0, secure_unpriv);
  CHECK(r.peek("GTZC1_TZSC", area) == (2u << 16 | 1u));

  r.write(
      "GTZC1_TZSC", config,
      bit("GTZC1_TZSC", config, "SREN")
          | bit("GTZC1_TZSC", config, "PRIV")
          | bit("GTZC1_TZSC", config, "SRLOCK"),
      secure_priv);
  CHECK(
      bus.read(
          backup_ns + 0x20, 4,
          bus_attrs{.by = initiator::dma, .secure = false, .privileged = false})
      == 0u);
  r.write("GTZC1_TZSC", area, 0, secure_priv);
  r.write("GTZC1_TZSC", config, 0, secure_priv);
  CHECK(r.peek("GTZC1_TZSC", area) == (2u << 16 | 1u));
  CHECK(ns_read(backup_ns + 0x20) == 0x2020'2020u);

  r.target->reset(popcorn::reset_kind::software);
  CHECK(r.peek("GTZC1_TZSC", config) == 0);
  r.write("GTZC1_TZSC", config, bit("GTZC1_TZSC", config, "SREN"), secure_priv);
  CHECK(r.peek("GTZC1_TZSC", config) == 0);
  CHECK(ns_read(backup_ns + 0x20) == 0u);
}

constexpr address       shcsr          = 0xe000'ed24;
constexpr address       shpr1          = 0xe000'ed18;
constexpr address       nonsecure_cfsr = 0xe002'ed28;
constexpr address       iser0          = 0xe000'e100;
constexpr address       ispr0          = 0xe000'e200;
constexpr address       ipr0           = 0xe000'e400;
constexpr std::uint32_t busfault_ena   = 1u << 17;
constexpr std::uint32_t sau_enable     = 1;

auto nonsecure_blocks(rig const& r) -> void
{
  REQUIRE(r.target->mem().write(sau_rnr, 4, 0));
  REQUIRE(r.target->mem().write(sau_rbar, 4, block_1));
  REQUIRE(r.target->mem().write(sau_rlar, 4, (block_3 + block_512 - 32) | 1u));
  REQUIRE(r.target->mem().write(sau_ctrl, 4, sau_enable));
  block_bits(r, block_2, false, true);
  std::uint32_t stack = block_2 + 0x1f0;
  REQUIRE(
      uc_reg_write(core::machine_internals::engine(*r.target), UC_ARM_REG_MSP_NS, &stack)
      == UC_ERR_OK);
}

auto secure_vectors(rig const& r, std::initializer_list<std::uint32_t> numbers) -> void
{
  put_code(r, handler_at, {bkpt});
  REQUIRE(r.target->mem().write(vectors_at, 4, stack_top));
  for(auto const number : numbers)
  {
    REQUIRE(r.target->mem().write(vectors_at + 4 * number, 4, handler_at | 1u));
  }
  REQUIRE(r.target->mem().write(vtor, 4, vectors_at));
  set(r, "msp", stack_top);
}

auto nonsecure_fetch_fault(address code, address entry, bool enabled) -> void
{
  rig const r{true, {.tzen = true}, popcorn::arch::fault_policy::enter_handler};
  nonsecure_blocks(r);
  secure_vectors(r, {3, 5});
  if(code == block_2)
  {
    block_bits(r, block_3, true, true);
    put_code(r, block_2 + block_512 - 4, {adds_r3_1, adds_r3_1});
  }
  else
  {
    put_code(r, code, {nop, bkpt});
  }
  if(enabled)
  {
    REQUIRE(r.target->mem().write(shcsr, 4, busfault_ena));
  }
  engine_secure(r, false);
  auto const run = run_from(r, entry, icount{8}, icount{8});
  CHECK(run.stopped.reason == popcorn::stop_reason::user);
  CHECK(get(r, "pc") == handler_at + 2);
  CHECK((get(r, "xpsr") & 0x1ff) == (enabled ? 5u : 3u));
  CHECK((word(r, hfsr) & hfsr_forced) == (enabled ? 0u : hfsr_forced));
  CHECK((word(r, cfsr) & cfsr_ibuserr) != 0);
  CHECK((word(r, nonsecure_cfsr) & cfsr_ibuserr) == 0);
}

TEST_CASE(
    "a Non-secure fetch the MPCBB refuses is a Secure BusFault with BFHFNMINS 0, escalated to "
    "HardFault_S without BUSFAULTENA, B3.11 RJQMQ and RDXYK")
{
  for(auto const enabled : {false, true})
  {
    CAPTURE(enabled);
    nonsecure_fetch_fault(block_1, block_1, enabled);
    nonsecure_fetch_fault(block_2, block_2 + block_512 - 4, enabled);
  }
}

TEST_CASE(
    "a Secure IRQ handler that makes the running Non-secure loop's block Secure faults the loop's "
    "next fetch after the return")
{
  rig const r{true};
  nonsecure_blocks(r);
  block_bits(r, block_3, false, true);
  std::uint32_t stack = block_3 + 0x1f0;
  REQUIRE(
      uc_reg_write(core::machine_internals::engine(*r.target), UC_ARM_REG_MSP_NS, &stack)
      == UC_ERR_OK);
  secure_vectors(r, {});
  auto const seccfgr0 = address_of("GTZC1_MPCBB1", "SECCFGR0");
  put_code(r, handler_at, {0x4802, 0x4903, str_r1_r0, bx_lr, nop, nop});
  REQUIRE(r.target->mem().write(handler_at + 12, 4, seccfgr0));
  REQUIRE(r.target->mem().write(handler_at + 16, 4, r.peek("GTZC1_MPCBB1", "SECCFGR0") | 4u));
  REQUIRE(r.target->mem().write(vectors_at + 4 * 16, 4, handler_at | 1u));
  put_code(r, block_2, {adds_r3_1, 0xe7fd});
  set(r, "r3", 0);
  engine_secure(r, false);
  set(r, "pc", block_2 | 1u);
  auto const warm = r.target->run_slice({.max_instructions = icount{40}});
  REQUIRE_FALSE(warm.stopped.has_value());
  REQUIRE(get(r, "r3") == 20);
  REQUIRE(r.target->mem().write(iser0, 4, 1));
  REQUIRE(r.target->mem().write(ispr0, 4, 1));
  auto const run = run_from(r, get(r, "pc") & ~1u, icount{200}, icount{200});
  expect_fetch_fault(run);
  CHECK((get(r, "pc") & ~1u) >= block_2);
  CHECK((get(r, "pc") & ~1u) < block_2 + 4);
  CHECK(get(r, "r3") == 20);
  CHECK(sram_flag(r) == false);
}

TEST_CASE(
    "a Secure fetch of a Non-secure block with GTZC_IRQn above BusFault runs the GTZC handler "
    "first, the fault after it")
{
  rig const r{true, {.tzen = true}, popcorn::arch::fault_policy::enter_handler};
  block_bits(r, block_1, false, true);
  constexpr address gtzc_handler = handler_at + 0x40;
  put_code(r, block_1, {nop, bkpt});
  secure_vectors(r, {5});
  put_code(r, gtzc_handler, {bkpt});
  REQUIRE(r.target->mem().write(vectors_at + 4 * (16 + gtzc_irq), 4, gtzc_handler | 1u));
  REQUIRE(r.target->mem().write(ipr0 + gtzc_irq, 1, 0));
  REQUIRE(r.target->mem().write(iser0, 4, 1u << gtzc_irq));
  REQUIRE(r.target->mem().write(shpr1 + 1, 1, 0x80));
  REQUIRE(r.target->mem().write(shcsr, 4, busfault_ena));
  r.write("GTZC1_TZIC", "IER4", ~0u, secure_priv);
  auto const first = run_from(r, block_1, icount{8}, icount{8});
  CHECK(first.stopped.reason == popcorn::stop_reason::user);
  CHECK(get(r, "pc") == gtzc_handler + 2);
  CHECK((get(r, "xpsr") & 0x1ff) == 16 + gtzc_irq);
  CHECK(sram_flag(r));
}
}
