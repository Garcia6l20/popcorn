#include "../../../native/src/core/machine_impl.hpp"
#include "gtzc.hpp"
#include "stm32_fixture.hpp"

#include <popcorn/arch/armv8m/armv8m.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/periph/model.hpp>
#include <popcorn/periph/stm32/board.hpp>
#include <popcorn/periph/stm32/builtin.hpp>

#include <doctest/doctest.h>

#include <unicorn/unicorn.h>

#include <algorithm>
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

  explicit rig(bool trustzone)
  {
    stm32::register_builtin_models();
    auto options = popcorn::testing::stm32_options();
    if(trustzone)
    {
      REQUIRE(stm32::configure_option_bytes(u575(), {.tzen = true}, options).has_value());
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

struct cpu_access
{
  std::uint16_t insn{};
  bool          nonsecure{};
  bool          unprivileged{};
};

constexpr address       code_at   = 0x2000'0000;
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

auto run_cpu(rig const& r, cpu_access const& access, address at, std::uint32_t value) -> void
{
  if(access.nonsecure)
  {
    REQUIRE(r.target->mem().write(sau_ctrl, 4, sau_allns));
    engine_secure(r, false);
  }
  set(r, "control", access.unprivileged ? 1 : 0);
  REQUIRE(r.target->mem().write(code_at, 2, access.insn));
  REQUIRE(r.target->mem().write(code_at + 2, 2, bkpt));
  set(r, "r0", at);
  set(r, "r1", value);
  set(r, "r2", 0xdead'beef);
  set(r, "pc", code_at | 1u);
  auto const result = r.target->run_slice({.max_instructions = icount{2}});
  REQUIRE(result.stopped.has_value());
  CHECK(result.stopped->reason == popcorn::stop_reason::user);
  CHECK(get(r, "pc") == code_at + 4);
  set(r, "control", 0);
  if(access.nonsecure)
  {
    engine_secure(r, true);
    REQUIRE(r.target->mem().write(sau_ctrl, 4, 0));
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
}
