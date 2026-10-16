#include "factories.hpp"

#include <popcorn/core/machine.hpp>
#include <popcorn/periph/model.hpp>
#include <popcorn/periph/stm32/board.hpp>
#include <popcorn/periph/stm32/builtin.hpp>

#include <array>
#include <expected>
#include <format>
#include <mutex>
#include <optional>
#include <ranges>
#include <span>
#include <string_view>
#include <vector>

namespace popcorn::periph::stm32
{
namespace
{
struct builtin
{
  std::string_view ip;
  std::string_view ip_version_glob;
  auto (*make)(board const& b) -> model_factory;
  std::string_view instance_glob = "*";
};

constexpr std::array<builtin, 27> builtins{
    {
     {"RCC", "STM32U5_rcc_*", [](board const& b) { return rcc_factory(b.rcc); }},
     {"ICACHE", "Icache_v1_0_0*", [](board const& b) { return icache_factory(b.caches); }},
     {"DCACHE", "STM32U5xx1_v0_0", [](board const& b) { return dcache_factory(b.caches); }},
     {"PWR", "STM32U5_pwr_*", [](board const& b) { return pwr_factory(b.pwr); }},
     {"flash", "v1.0.0_U5_Cube", [](board const& b) { return flash_factory(b.flash); }},
     {"GPIO", "STM32U5x_gpio_*", [](board const&) { return gpio_factory(); }, "GPIO?"},
     {"EXTI", "", [](board const&) { return exti_factory(); }},
     {"GPDMA", "STM32U5_dma3_*", [](board const&) { return gpdma_factory(); }},
     {"LPDMA", "STM32U5_dma3_*", [](board const&) { return lpdma_factory(); }},
     {"USART", "sci3_v2*", [](board const& b) { return usart_factory(b); }},
     {"UART", "sci2_*", [](board const& b) { return usart_factory(b); }},
     {"LPUART", "sci3_v1*", [](board const& b) { return usart_factory(b); }},
     {"TIM1_8U5", "gptimer2_v4_x*", [](board const&) { return tim_factory(); }},
     {"TIM6_7U5", "gptimer2_v4_x*", [](board const&) { return tim_factory(); }},
     {"IWDG", "iwdg1_v3*", [](board const&) { return iwdg_factory(); }},
     {"RTC", "rtc2_v2_U5*", [](board const& b) { return rtc_factory(b.rtc_initial); }},
     {"TAMP", "STM32L5_rtc2_v2_8*", [](board const&) { return tamp_factory(); }},
     {"SPI", "spi2s3_v1*", [](board const& b) { return spi_factory(b); }},
     {"I2C", "i2c2_v1_1U5*", [](board const& b) { return i2c_factory(b); }},
     {"SAI", "sai1_L5*", [](board const& b) { return sai_factory(b); }},
     {"USB_OTG_FS", "otgfs1_v3_0_Cube", [](board const& b) { return otg_fs_factory(b); }},
     {"ADC", "aditf5_40v1_U5_Cube", [](board const& b) { return adc_factory(b.supply); }},
     {"ADC12", "*", [](board const&) { return adc_common_factory(); }},
     {"DBGMCU", "", [](board const&) { return dbgmcu_factory(); }},
     {"CRC", "integtest1_v2_0", [](board const&) { return crc_factory(); }},
     {"HASH", "hash1_v2_2*", [](board const&) { return hash_factory(); }},
     {"GTZC", "tzsc1_v2_2*", [](board const&) { return gtzc_factory(); }},
     }
};

constexpr auto u5_idau = std::to_array<idau_region>({
    // Code area, Non-secure flash and SRAM aliases, RM0456 2.2 Table 3.
    {.start = 0x0000'0000, .end = 0x0c00'0000,   .nsc = false, .exempt = false},
    // Code area, Secure flash alias, Non-secure callable, RM0456 2.2 Table 3.
    {.start = 0x0c00'0000, .end = 0x1000'0000,   .nsc = true,  .exempt = false},
    // SRAM area, Non-secure SRAM aliases, RM0456 2.2 Table 3.
    {.start = 0x1000'0000, .end = 0x3000'0000,   .nsc = false, .exempt = false},
    // SRAM area, Secure SRAM aliases, Non-secure callable, RM0456 2.2 Table 3.
    {.start = 0x3000'0000, .end = 0x4000'0000,   .nsc = true,  .exempt = false},
    // Peripherals, Non-secure aliases, RM0456 2.2 Table 3.
    {.start = 0x4000'0000, .end = 0x5000'0000,   .nsc = false, .exempt = false},
    // Peripherals, Secure aliases, Non-secure callable, RM0456 2.2 Table 3.
    {.start = 0x5000'0000, .end = 0x6000'0000,   .nsc = true,  .exempt = false},
    // External memories, Non-secure, RM0456 2.2 Table 3.
    {.start = 0x6000'0000, .end = 0xe000'0000,   .nsc = false, .exempt = false},
    // Cortex-M33 private peripheral bus, Non-secure, RM0456 2.2 Figures 3 to 6.
    {.start = 0xe000'0000, .end = 0xe004'0000,   .nsc = false, .exempt = false},
    // External PPB, exempt: a board's Non-secure access to the CTI at 0xE0042000 with no SAU
    // region works, QEMU's SSE-200 IDAU exempts the same range, ARMv8-M B10.2 exempts only
    // TPIU, ETM and the ROM table.
    {.start = 0xe004'0000, .end = 0xe010'0000,   .nsc = false, .exempt = true },
    // System area up to the top of the address space, Non-secure, RM0456 2.2 Figures 3 to 6.
    {.start = 0xe010'0000, .end = 0x1'0000'0000, .nsc = false, .exempt = false},
});

struct idau_family
{
  std::string_view             part_glob;
  std::span<idau_region const> ranges;
};

constexpr std::array<idau_family, 1> idau_maps{
    {
     {"STM32U5*", u5_idau},
     }
};

constexpr std::array<sram_blocks, 4> u575_mpcbb{
    {
     {.controller        = "GTZC1_MPCBB1",
         .start             = 0x2000'0000,
         .size              = 0x3'0000,
         .block_size        = 512,
         .superblock_blocks = 32},
     {.controller        = "GTZC1_MPCBB2",
         .start             = 0x2003'0000,
         .size              = 0x1'0000,
         .block_size        = 512,
         .superblock_blocks = 32},
     {.controller        = "GTZC1_MPCBB3",
         .start             = 0x2004'0000,
         .size              = 0x8'0000,
         .block_size        = 512,
         .superblock_blocks = 32},
     {.controller        = "GTZC2_MPCBB4",
         .start             = 0x2800'0000,
         .size              = 0x4000,
         .block_size        = 512,
         .superblock_blocks = 32},
     }
};

struct mpcbb_family
{
  std::string_view             part_glob;
  std::span<sram_blocks const> controllers;
};

constexpr std::array<mpcbb_family, 2> mpcbb_maps{
    {
     {"STM32U575*", u575_mpcbb},
     {"STM32U585*", u575_mpcbb},
     }
};

auto default_board() -> board const&
{
  static board const instance{};
  return instance;
}
}

void register_builtin_models()
{
  static std::once_flag once;
  std::call_once(once, [] {
    for(auto const& entry : builtins)
    {
      register_model(entry.ip, entry.ip_version_glob, entry.make(default_board()));
    }
  });
}

auto factories(board const& b, part_description const& part)
    -> std::map<std::string, model_factory, std::less<>>
{
  std::map<std::string, model_factory, std::less<>> out;
  for(auto const& unit : part.peripherals)
  {
    if(unit.alias_of)
    {
      continue;
    }
    for(auto const& entry : builtins)
    {
      if(entry.ip == unit.ip
         and glob_match(entry.ip_version_glob, unit.ip_version)
         and glob_match(entry.instance_glob, unit.name))
      {
        out.emplace(unit.name, entry.make(b));
        break;
      }
    }
  }
  return out;
}

auto idau_map(part_description const& part) -> std::optional<std::span<idau_region const>>
{
  for(auto const& family : idau_maps)
  {
    if(glob_match(family.part_glob, part.part))
    {
      return family.ranges;
    }
  }
  return std::nullopt;
}

auto mpcbb_geometry(part_description const& part, std::string_view controller)
    -> std::optional<sram_blocks>
{
  for(auto const& family : mpcbb_maps)
  {
    if(glob_match(family.part_glob, part.part))
    {
      auto const found =
          std::ranges::find(family.controllers, controller, &sram_blocks::controller);
      if(found != family.controllers.end())
      {
        return *found;
      }
    }
  }
  return std::nullopt;
}

auto configure_option_bytes(
    part_description const& part,
    option_bytes const&     wanted,
    core::machine_options&  options) -> std::expected<void, option_refusal>
{
  auto const refuse = [](std::string key, std::string expected) {
    return std::unexpected{
        option_refusal{.key = std::move(key), .expected = std::move(expected)}
    };
  };
  if(wanted.tzen and not part.core.trustzone)
  {
    return refuse(
        "option_bytes.tzen",
        std::format("a part with a security extension, {} has none", part.part));
  }
  auto const map = idau_map(part);
  if(wanted.tzen and not map)
  {
    return refuse(
        "option_bytes.tzen",
        std::format("a part whose family has an IDAU table, {} has none", part.part));
  }
  auto const boot = check_option_bytes(part, wanted);
  if(not boot)
  {
    return std::unexpected{boot.error()};
  }
  auto const unit = std::ranges::find_if(part.peripherals, [](peripheral_desc const& p) {
    return p.ip == "flash" and not p.alias_of;
  });
  if(options.models.contains(unit->name))
  {
    return refuse(
        "option_bytes",
        std::format(
            "the built-in model of {}, not a model of machine_options::models", unit->name));
  }
  board with_options;
  with_options.flash.option_bytes = wanted;
  auto all                        = factories(with_options, part);
  auto found                      = all.find(unit->name);
  if(found == all.end())
  {
    return refuse("option_bytes", std::format("a {} the built-in FLASH model serves", unit->name));
  }
  options.models.emplace(unit->name, std::move(found->second));
  if(not options.vector_table)
  {
    options.vector_table = *boot;
  }
  options.trustzone = wanted.tzen;
  options.idau = wanted.tzen ? *map | std::ranges::to<std::vector>() : std::vector<idau_region>{};
  return {};
}
}
