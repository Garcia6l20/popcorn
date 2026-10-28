#include "factories.hpp"

#include <popcorn/core/errors.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/periph/model.hpp>
#include <popcorn/periph/stm32/board.hpp>
#include <popcorn/periph/stm32/builtin.hpp>

#include <array>
#include <expected>
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <span>
#include <string>
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
  std::string_view part_glob     = "*";
};

constexpr std::array<builtin, 57> builtins{
    {
     {"RCC", "STM32U5_rcc_*", [](board const& b) { return rcc_factory(b.rcc); }},
     {"RCC", "STM32H5_rcc_*", [](board const& b) { return rcc_h5_factory(b.rcc); }},
     {"ICACHE", "Icache_v1_0_0*", [](board const& b) { return icache_factory(b.caches); }},
     {"DCACHE", "STM32U5xx1_v0_0", [](board const& b) { return dcache_factory(b.caches); }},
     {"PWR", "STM32U5_pwr_*", [](board const& b) { return pwr_factory(b.pwr); }},
     {"PWR", "STM32H5_pwr_*", [](board const& b) { return pwr_h5_factory(b.pwr); }},
     {"flash", "v1.0.0_U5_Cube", [](board const& b) { return flash_factory(b.flash); }},
     {"flash", "v1.0.0_H5_Cube", [](board const& b) { return flash_h5_factory(b.flash); }},
     {.ip              = "GPIO",
         .ip_version_glob = "STM32U5x_gpio_*",
         .make            = [](board const&) { return gpio_factory(u575_gpio); },
         .instance_glob   = "GPIO?",
         .part_glob       = "STM32U575*"},
     {.ip              = "GPIO",
         .ip_version_glob = "STM32U5x_gpio_*",
         .make            = [](board const&) { return gpio_factory(u575_gpio); },
         .instance_glob   = "GPIO?",
         .part_glob       = "STM32U585*"},
     {"GPIO", "STM32U5x_gpio_*", [](board const&) { return gpio_factory(u5_gpio); }, "GPIO?"},
     {"GPIO", "STM32H56x_gpio_*", [](board const&) { return gpio_factory(h56x_gpio); }, "GPIO?"},
     {"EXTI", "", [](board const&) { return exti_factory(); }},
     {"GPDMA", "STM32U5_dma3_*", [](board const&) { return gpdma_factory(); }},
     {"GPDMA", "*STM32H5_dma3_*", [](board const&) { return gpdma_h5_factory(); }},
     {"LPDMA", "STM32U5_dma3_*", [](board const&) { return lpdma_factory(); }},
     {.ip              = "USART",
         .ip_version_glob = "sci3_v2*",
         .make            = [](board const& b) { return usart_factory(b, with_views_usart); },
         .part_glob       = "STM32U5*"},
     {.ip              = "USART",
         .ip_version_glob = "sci3_v2*",
         .make            = [](board const& b) { return usart_factory(b, with_views_usart); },
         .part_glob       = "STM32H5*"},
     {.ip              = "UART",
         .ip_version_glob = "sci2_*",
         .make            = [](board const& b) { return usart_factory(b, with_views_usart); },
         .part_glob       = "STM32U5*"},
     {.ip              = "UART",
         .ip_version_glob = "sci2_*",
         .make            = [](board const& b) { return usart_factory(b, with_views_usart); },
         .part_glob       = "STM32H5*"},
     {.ip              = "LPUART",
         .ip_version_glob = "sci3_v1*",
         .make            = [](board const& b) { return usart_factory(b, u5_lpuart); },
         .part_glob       = "STM32U5*"},
     {.ip              = "LPUART",
         .ip_version_glob = "sci3_v1*",
         .make            = [](board const& b) { return usart_factory(b, h5_lpuart); },
         .part_glob       = "STM32H5*"},
     {.ip              = "TIM1_8U5",
         .ip_version_glob = "gptimer2_v4_x*",
         .make            = [](board const&) { return tim_factory(u5_tim); },
         .part_glob       = "STM32U5*"},
     {.ip              = "TIM6_7U5",
         .ip_version_glob = "gptimer2_v4_x*",
         .make            = [](board const&) { return tim_factory(u5_tim); },
         .part_glob       = "STM32U5*"},
     {.ip              = "TIM1_8H5",
         .ip_version_glob = "gptimer2_v4_x*",
         .make            = [](board const&) { return tim_factory(h5_tim); },
         .part_glob       = "STM32H5*"},
     {.ip              = "TIM6_7H5",
         .ip_version_glob = "gptimer2_v4_x*",
         .make            = [](board const&) { return tim_factory(h5_tim); },
         .part_glob       = "STM32H5*"},
     {"IWDG", "iwdg1_v3*", [](board const&) { return iwdg_factory(); }},
     {.ip              = "RTC",
         .ip_version_glob = "rtc2_v2_U5*",
         .make            = [](board const& b) { return rtc_factory(b.rtc_initial, u5_rtc); },
         .part_glob       = "STM32U5*"},
     {.ip              = "RTC",
         .ip_version_glob = "rtc2_v2_U5*",
         .make            = [](board const& b) { return rtc_factory(b.rtc_initial, h5_rtc); },
         .part_glob       = "STM32H5*"},
     {.ip              = "TAMP",
         .ip_version_glob = "STM32L5_rtc2_v2_8*",
         .make            = [](board const&) { return tamp_factory(u5_rtc); },
         .part_glob       = "STM32U5*"},
     {.ip              = "TAMP",
         .ip_version_glob = "STM32L5_rtc2_v2_8*",
         .make            = [](board const&) { return tamp_factory(h5_rtc); },
         .part_glob       = "STM32H5*"},
     {.ip              = "SPI",
         .ip_version_glob = "spi2s3_v1*",
         .make            = [](board const& b) { return spi_factory(b, u5_spi); },
         .part_glob       = "STM32U5*"},
     {.ip              = "SPI",
         .ip_version_glob = "spi2s3_v2*",
         .make            = [](board const& b) { return spi_factory(b, h5_spi); },
         .part_glob       = "STM32H5*"},
     {.ip              = "I2C",
         .ip_version_glob = "i2c2_v1_1U5*",
         .make            = [](board const& b) { return i2c_factory(b, u5_i2c); },
         .part_glob       = "STM32U5*"},
     {.ip              = "I2C",
         .ip_version_glob = "i2c2_v1_1U5*",
         .make            = [](board const& b) { return i2c_factory(b, h5_i2c); },
         .part_glob       = "STM32H5*"},
     {.ip              = "SAI",
         .ip_version_glob = "sai1_L5*",
         .make            = [](board const& b) { return sai_factory(b, u5_sai); },
         .part_glob       = "STM32U5*"},
     {.ip              = "SAI",
         .ip_version_glob = "sai1_L5*",
         .make            = [](board const& b) { return sai_factory(b, h5_sai); },
         .part_glob       = "STM32H5*"},
     {"MDF", "v1_0_Cube", [](board const&) { return mdf_factory(); }},
     {"USB_OTG_FS", "otgfs1_v3_0_Cube", [](board const& b) { return otg_fs_factory(b); }},
     {"USB", "usbfs_v1*", [](board const& b) { return usb_drd_factory(b); }},
     {"ADC", "aditf5_40v1_U5_Cube", [](board const& b) { return adc_factory(b.supply); }},
     {"ADC12", "*", [](board const&) { return adc_common_factory(); }},
     {.ip              = "ADC",
         .ip_version_glob = "aditf5_v3_0_H5*",
         .make            = [](board const& b) { return adc_h5_factory(b.supply); },
         .part_glob       = "STM32H5*"},
     {.ip              = "ADCC",
         .ip_version_glob = "*",
         .make            = [](board const&) { return adc_common_h5_factory(); },
         .part_glob       = "STM32H5*"},
     {"DBGMCU", "", [](board const&) { return dbgmcu_factory(); }},
     {"DBG", "", [](board const&) { return dbgmcu_factory(); }},
     {"CRC", "integtest1_v2_0", [](board const&) { return crc_factory(); }},
     {"HASH", "hash1_v2_2*", [](board const&) { return hash_factory(); }},
     {.ip              = "HASH",
         .ip_version_glob = "hash1_v4*",
         .make            = [](board const&) { return hash_h5_factory(); },
         .part_glob       = "STM32H52*"},
     {.ip              = "HASH",
         .ip_version_glob = "hash1_v4*",
         .make            = [](board const&) { return hash_h5_factory(); },
         .part_glob       = "STM32H53*"},
     {.ip              = "HASH",
         .ip_version_glob = "hash1_v4*",
         .make            = [](board const&) { return hash_h5_factory(); },
         .part_glob       = "STM32H56*"},
     {.ip              = "HASH",
         .ip_version_glob = "hash1_v4*",
         .make            = [](board const&) { return hash_h5_factory(); },
         .part_glob       = "STM32H57*"},
     {"RNG", "rng1_v4_1", [](board const& b) { return rng_factory(b.rng); }},
     {"SYS", "STM32U5x_sys_*", [](board const&) { return syscfg_factory(); }},
     {.ip              = "GTZC",
         .ip_version_glob = "tzsc1_v2_2*",
         .make            = [](board const&) { return gtzc_factory(u5_gtzc); },
         .part_glob       = "STM32U5*"},
     {.ip              = "GTZC",
         .ip_version_glob = "tzsc1_v2_2*",
         .make            = [](board const&) { return gtzc_factory(h5_gtzc); },
         .part_glob       = "STM32H5*"},
     {"SBS", "sbs_v1_1*", [](board const&) { return sbs_factory(); }},
     }
};

/** @brief IDAU map of the STM32U5, RM0456 2.2 Table 3, and of the STM32H5, whose RM0481 2.2
 *         Figures 2 to 4 give the same boundaries. */
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

constexpr std::array<idau_family, 2> idau_maps{
    {
     {"STM32U5*", u5_idau},
     {"STM32H5*", u5_idau},
     }
};

struct debug_block
{
  std::string_view name;
  address          start{};
  std::uint32_t    size{};
};

// External PPB CoreSight blocks without a model, RM0456 75.5 Tables 797 and 798, 4 KB each.
// DBGMCU at 0xE0044000 has its own model. 0xE0043000 is reserved and stays unmapped.
constexpr auto u5_debug_blocks = std::to_array<debug_block>({
    {.name = "TPIU",          .start = 0xe004'0000, .size = 0x1000},
    {.name = "ETM",           .start = 0xe004'1000, .size = 0x1000},
    {.name = "CTI",           .start = 0xe004'2000, .size = 0x1000},
    {.name = "MCU_ROM_TABLE", .start = 0xe00f'e000, .size = 0x1000},
    {.name = "CPU_ROM_TABLE", .start = 0xe00f'f000, .size = 0x1000},
});

struct debug_family
{
  std::string_view             part_glob;
  std::span<debug_block const> blocks;
};

// The H5 has the same blocks at the same addresses, RM0481 66.5 Tables 870 and 871. Its DBGMCU
// sits at 0xE00E4000 for the debugger and 0x44024000 for the CPU, outside these ranges.
constexpr std::array<debug_family, 2> debug_maps{
    {
     {"STM32U5*", u5_debug_blocks},
     {"STM32H5*", u5_debug_blocks},
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

constexpr std::array<sram_blocks, 3> h573_mpcbb{
    {
     {.controller        = "GTZC1_MPCBB1",
         .start             = 0x2000'0000,
         .size              = 0x4'0000,
         .block_size        = 512,
         .superblock_blocks = 32},
     {.controller        = "GTZC1_MPCBB2",
         .start             = 0x2004'0000,
         .size              = 0x1'0000,
         .block_size        = 512,
         .superblock_blocks = 32},
     {.controller        = "GTZC1_MPCBB3",
         .start             = 0x2005'0000,
         .size              = 0x5'0000,
         .block_size        = 512,
         .superblock_blocks = 32},
     }
};

constexpr std::array<mpcbb_family, 5> mpcbb_maps{
    {
     {"STM32U575*", u575_mpcbb},
     {"STM32U585*", u575_mpcbb},
     {"STM32H562*", h573_mpcbb},
     {"STM32H563*", h573_mpcbb},
     {"STM32H573*", h573_mpcbb},
     }
};

auto default_board() -> board const&
{
  static board const instance{};
  return instance;
}

auto select_row(
    std::string_view ip,
    std::string_view version,
    std::string_view part,
    std::string_view instance) -> builtin const*
{
  auto const same_version = [&](builtin const& entry) {
    return entry.ip == ip and glob_match(entry.ip_version_glob, version);
  };
  auto const found = std::ranges::find_if(builtins, [&](builtin const& entry) {
    return same_version(entry)
       and glob_match(entry.part_glob, part)
       and glob_match(entry.instance_glob, instance);
  });
  if(found != builtins.end())
  {
    return &*found;
  }
  if(std::ranges::any_of(builtins, same_version)
     and not std::ranges::any_of(builtins, [&](builtin const& entry) {
           return same_version(entry) and glob_match(entry.part_glob, part);
         }))
  {
    throw config_error(
        std::string{part}, 0, std::format("peripherals.{}", instance),
        std::format("no model row of the {} version {} serves the part {}", ip, version, part));
  }
  return nullptr;
}
}

void register_builtin_models()
{
  static std::once_flag once;
  std::call_once(once, [] {
    auto made = std::make_shared<std::vector<model_factory>>();
    for(auto const& entry : builtins)
    {
      made->push_back(entry.make(default_board()));
    }
    for(auto const& entry : builtins)
    {
      auto const first = std::ranges::find_if(builtins, [&](builtin const& other) {
        return other.ip == entry.ip and other.ip_version_glob == entry.ip_version_glob;
      });
      if(&*first != &entry)
      {
        continue;
      }
      register_model(
          entry.ip, entry.ip_version_glob,
          [made](model_context& ctx, peripheral_desc const& desc) -> std::shared_ptr<model> {
            auto const* const row = select_row(desc.ip, desc.ip_version, ctx.part.part, desc.name);
            return row == nullptr ? nullptr : (*made)[row - builtins.data()](ctx, desc);
          });
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
    if(auto const* const row = select_row(unit.ip, unit.ip_version, part.part, unit.name))
    {
      out.emplace(unit.name, row->make(b));
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

auto raz_wi_map(part_description const& part) -> std::vector<address_range>
{
  auto const family = std::ranges::find_if(
      debug_maps, [&](debug_family const& f) { return glob_match(f.part_glob, part.part); });
  if(family == debug_maps.end())
  {
    return {};
  }
  return family->blocks
       | std::views::transform([](debug_block const& block) {
           return address_range{
               .name = std::string{block.name}, .start = block.start, .size = block.size};
         })
       | std::ranges::to<std::vector>();
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
