#include "adc_rows.hpp"
#include "caches_rows.hpp"
#include "crc_rows.hpp"
#include "crs_rows.hpp"
#include "dac_rows.hpp"
#include "dbgmcu_rows.hpp"
#include "dma_rows.hpp"
#include "exti_rows.hpp"
#include "flash_rows.hpp"
#include "gpio_rows.hpp"
#include "gtzc_rows.hpp"
#include "hash_rows.hpp"
#include "i2c_rows.hpp"
#include "iwdg_rows.hpp"
#include "lptim_rows.hpp"
#include "mdf_rows.hpp"
#include "pwr_rows.hpp"
#include "rcc_rows.hpp"
#include "rng_rows.hpp"
#include "row_of.hpp"
#include "rows.hpp"
#include "rtc_rows.hpp"
#include "sai_rows.hpp"
#include "sbs_rows.hpp"
#include "spi_rows.hpp"
#include "syscfg_rows.hpp"
#include "tim_rows.hpp"
#include "usart_rows.hpp"
#include "usb_rows.hpp"

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

auto builtins() -> std::span<builtin const>
{
  static auto const table = [] {
    std::vector<builtin> all;
    for(auto const rows :
        {rcc_rows(),   pwr_rows(),    flash_rows(),       caches_rows(), crs_rows(),    gpio_rows(),
         exti_rows(),  syscfg_rows(), dma_classic_rows(), gpdma_rows(),  lptim_rows(),  tim_rows(),
         usart_rows(), i2c_rows(),    spi_rows(),         usb_rows(),    adc_rows(),    dac_rows(),
         iwdg_rows(),  rtc_rows(),    sai_rows(),         mdf_rows(),    dbgmcu_rows(), crc_rows(),
         hash_rows(),  rng_rows(),    gtzc_rows(),        sbs_rows()})
    {
      all.insert(all.end(), rows.begin(), rows.end());
    }
    return all;
  }();
  return table;
}

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

/** @brief External PPB blocks of the Cortex-M3 and Cortex-M4 parts without a model, 4 KB each:
 *         the TPIU at 0xE0040000, the ETM at 0xE0041000 and the ROM table at 0xE00FF000. STM32L1:
 *         RM0038 30.17.9, 30.15 and 30.6. STM32L4: RM0351 48.17.9 Table 349, 48.15.3 Table 345
 *         and 48.6.4. DBGMCU at 0xE0042000 has its own row. */
constexpr auto cm_debug_blocks = std::to_array<debug_block>({
    {.name = "TPIU",      .start = 0xe004'0000, .size = 0x1000},
    {.name = "ETM",       .start = 0xe004'1000, .size = 0x1000},
    {.name = "ROM_TABLE", .start = 0xe00f'f000, .size = 0x1000},
});

struct debug_family
{
  std::string_view             part_glob;
  std::span<debug_block const> blocks;
};

// The H5 has the same blocks at the same addresses, RM0481 66.5 Tables 870 and 871. Its DBGMCU
// sits at 0xE00E4000 for the debugger and 0x44024000 for the CPU, outside these ranges.
constexpr std::array<debug_family, 4> debug_maps{
    {
     {"STM32U5*", u5_debug_blocks},
     {"STM32H5*", u5_debug_blocks},
     {"STM32L1*", cm_debug_blocks},
     {"STM32L4*", cm_debug_blocks},
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

auto scope_of(std::span<std::string_view const> globs) -> std::string
{
  return globs | std::views::join_with(',') | std::ranges::to<std::string>();
}

auto row_label(builtin const& entry) -> std::string
{
  return std::format(
      "{}/{}/{}/{}", entry.ip, entry.ip_version_glob.empty() ? "-" : entry.ip_version_glob,
      scope_of(entry.part_globs), scope_of(entry.instance_globs));
}

auto select_row(
    std::string_view ip,
    std::string_view version,
    std::string_view part,
    std::string_view instance) -> builtin const*
{
  return select_in(builtins(), ip, version, part, instance);
}
}

auto rows() -> std::vector<row_info>
{
  return builtins()
       | std::views::transform([](builtin const& entry) {
           return row_info{
               .label      = row_label(entry),
               .part_globs = entry.part_globs | std::ranges::to<std::vector<std::string>>()};
         })
       | std::ranges::to<std::vector>();
}

auto resolve(part_description const& part) -> std::vector<resolution>
{
  std::vector<resolution> out;
  for(auto const& unit : part.peripherals)
  {
    if(unit.alias_of)
    {
      continue;
    }
    resolution found;
    found.instance   = unit.name;
    found.ip         = unit.ip;
    found.ip_version = unit.ip_version;
    for(auto const index : std::views::iota(std::size_t{0}, builtins().size()))
    {
      if(row_matches(builtins()[index], unit.ip, unit.ip_version, part.part, unit.name))
      {
        found.matches.push_back(index);
      }
    }
    if(not found.matches.empty())
    {
      found.row       = found.matches.front();
      found.row_label = row_label(builtins()[*found.row]);
      found.factory   = builtins()[*found.row].factory;
      found.variant   = builtins()[*found.row].variant;
      found.reads     = builtins()[*found.row].reads;
      found.source    = row_source::row;
    }
    else
    {
      found.source = part_refused(builtins(), unit.ip, unit.ip_version, part.part)
                       ? row_source::none_part_refused
                       : row_source::none_no_row;
    }
    out.push_back(std::move(found));
  }
  return out;
}

void register_builtin_models()
{
  static std::once_flag once;
  std::call_once(once, [] {
    auto made = std::make_shared<std::vector<model_factory>>();
    for(auto const& entry : builtins())
    {
      made->push_back(entry.make(default_board()));
    }
    for(auto const& entry : builtins())
    {
      auto const first = std::ranges::find_if(builtins(), [&](builtin const& other) {
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
            if(row == nullptr)
            {
              return nullptr;
            }
            auto& make = (*made)[row - builtins().data()];
            return make ? make(ctx, desc) : nullptr;
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
      if(auto made = row->make(b))
      {
        out.emplace(unit.name, std::move(made));
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
  auto const program = flash_unit(part);
  if(not program)
  {
    return refuse(
        "option_bytes",
        std::format("a part with a flash region its FLASH controls, not {}", part.part));
  }
  auto const* const unit = program->unit;
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
