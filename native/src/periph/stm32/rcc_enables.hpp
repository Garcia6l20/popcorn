#pragma once

#include <popcorn/core/description.hpp>

#include <algorithm>
#include <array>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace popcorn::periph::stm32
{
/**
 * @brief Instances whose RCC bus clock enable is not named `<instance>EN` in at least one
 *        family: the STM32U5 RM0456 11.8.29 to 11.8.36, the STM32H5 RM0481 11.8.26 to 11.8.32,
 *        the STM32L1 RM0038 6.3.8 to 6.3.10 and the STM32L47x/L48x RM0351 6.4.16 to 6.4.21. An
 *        instance may have several rows, one per family spelling, searched in order after
 *        `<instance>EN`, so a row written for one family never shadows the bit another family
 *        names after the instance, such as the STM32U5 OPAMPEN against the STM32L1 COMPEN. A bit
 *        shared by several instances, such as the STM32H5 ADC12EN or the STM32L4 SYSCFGEN, has
 *        one row per instance.
 *
 *        Inferred rows, the manual naming only the host block's clock: the GTZC TZIC and MPCBB
 *        blocks on their GTZC or TZSC enable, the OCTOSPI delay blocks on their OCTOSPI enable,
 *        the STM32U5 SDMMC delay blocks on their SDMMC enable, STM32U5 FDCAN1_RAM on FDCAN1EN,
 *        STM32L1 OPAMP on COMPEN and STM32L1 USB_SRAM on USBEN. The STM32H5 SDMMC delay blocks
 *        follow RM0481 11.8.28, which names them with SDMMC1EN and SDMMC2EN.
 */
inline constexpr auto enable_bit_names =
    std::to_array<std::pair<std::string_view, std::string_view>>({
        {"ADC",          "ADC1EN"     },
        {"ADC1",         "ADC12EN"    },
        {"ADC2",         "ADC12EN"    },
        {"ADCC",         "ADC12EN"    },
        {"ADC1",         "ADCEN"      },
        {"ADC2",         "ADCEN"      },
        {"ADC3",         "ADCEN"      },
        {"ADC_Common",   "ADCEN"      },
        {"COMP",         "COMPEN"     },
        {"COMP",         "SYSCFGEN"   },
        {"DAC",          "DAC12EN"    },
        {"DCACHE",       "DCACHE1EN"  },
        {"DCMI",         "DCMI_PSSIEN"},
        {"PSSI",         "DCMI_PSSIEN"},
        {"DLYBOS1",      "OCTOSPI1EN" },
        {"DLYBOS2",      "OCTOSPI2EN" },
        {"DLYBSD1",      "SDMMC1EN"   },
        {"DLYBSD2",      "SDMMC2EN"   },
        {"FDCAN1",       "FDCAN12EN"  },
        {"FDCAN2",       "FDCAN12EN"  },
        {"FDCAN1_RAM",   "FDCAN1EN"   },
        {"FIREWALL",     "FWEN"       },
        {"FLASH",        "FLITFEN"    },
        {"FMC",          "FSMCEN"     },
        {"GTZC1_TZSC",   "TZSC1EN"    },
        {"GTZC1_TZIC",   "TZSC1EN"    },
        {"GTZC1_MPCBB1", "TZSC1EN"    },
        {"GTZC1_MPCBB2", "TZSC1EN"    },
        {"GTZC1_MPCBB3", "TZSC1EN"    },
        {"GTZC1_TZSC",   "GTZC1EN"    },
        {"GTZC1_TZIC",   "GTZC1EN"    },
        {"GTZC1_MPCBB1", "GTZC1EN"    },
        {"GTZC1_MPCBB2", "GTZC1EN"    },
        {"GTZC1_MPCBB3", "GTZC1EN"    },
        {"GTZC2_TZSC",   "GTZC2EN"    },
        {"GTZC2_TZIC",   "GTZC2EN"    },
        {"GTZC2_MPCBB4", "GTZC2EN"    },
        {"I3C",          "I3C1EN"     },
        {"LPUART",       "LPUART1EN"  },
        {"OCTOSPI",      "OCTOSPI1EN" },
        {"OPAMP",        "COMPEN"     },
        {"OTG_FS",       "OTGEN"      },
        {"OTG_FS",       "OTGFSEN"    },
        {"QUADSPI",      "QSPIEN"     },
        {"RI",           "COMPEN"     },
        {"RTC",          "RTCAPBEN"   },
        {"TAMP",         "RTCAPBEN"   },
        {"UCPD1",        "UCPDEN"     },
        {"USB",          "USBFSEN"    },
        {"USB_SRAM",     "USBEN"      },
        {"VREFBUF",      "VREFEN"     },
        {"VREFBUF",      "SYSCFGEN"   },
});

/** @brief Where an instance's RCC bus clock enable lives in the RCC description. */
struct enable_bit
{
  /** @brief Name of the register, as the description spells it. */
  std::string reg;
  /** @brief Field name of the bit. */
  std::string field;
};

/**
 * @brief Every field name the RCC enable of @p instance may take, in search order:
 *        `<instance>EN`, then the names of enable_bit_names in table order.
 * @param instance Instance name as the description spells it.
 */
[[nodiscard]] inline auto enable_bit_candidates(std::string_view instance)
    -> std::vector<std::string>
{
  std::vector<std::string> names{std::string{instance} + "EN"};
  for(auto const& row : enable_bit_names)
  {
    if(row.first == instance and not std::ranges::contains(names, row.second))
    {
      names.emplace_back(row.second);
    }
  }
  return names;
}

/** @brief Whether enable_bit_names has a row for @p instance. */
[[nodiscard]] inline auto has_enable_row(std::string_view instance) -> bool
{
  return std::ranges::contains(
      enable_bit_names, instance, &std::pair<std::string_view, std::string_view>::first);
}

/**
 * @brief Whether @p name is an RCC enable register, which holds `ENR`. The sleep-mode enable
 *        registers, `LPENR` and `SMENR`, match too: their fields end in `LPEN` or `SMEN`, so no
 *        candidate of enable_bit_candidates() names one.
 */
[[nodiscard]] inline auto is_enable_register(std::string_view name) -> bool
{
  return name.contains("ENR");
}

/**
 * @brief The RCC bus clock enable of @p instance. Every candidate of enable_bit_candidates() is
 *        searched in the enable registers of @p rcc, is_enable_register(), in candidate order,
 *        then every candidate in @p extra, the registers that hold an enable outside them, such
 *        as the RTCEN of the STM32L1 RCC_CSR and the STM32L4 RCC_BDCR (RM0038 5.1.3 and RM0351
 *        5.1.5 list it as a step of RTC register access).
 * @param rcc Description of the RCC peripheral.
 * @param instance Instance name as the description spells it.
 * @param extra Names or aliases of the other registers searched, after the enable registers.
 * @return The register name and field, nullopt when no searched register has it.
 */
[[nodiscard]] inline auto enable_bit_of(
    peripheral_desc const&            rcc,
    std::string_view                  instance,
    std::span<std::string_view const> extra) -> std::optional<enable_bit>
{
  auto const holds = [](register_desc const& reg, std::string_view name) {
    return std::ranges::contains(reg.fields, name, &field_desc::name);
  };
  auto const candidates = enable_bit_candidates(instance);
  for(auto const& name : candidates)
  {
    for(auto const& reg : rcc.registers)
    {
      if(not reg.alternate_of and is_enable_register(reg.name) and holds(reg, name))
      {
        return enable_bit{.reg = reg.name, .field = name};
      }
    }
  }
  for(auto const& name : candidates)
  {
    for(auto const other : extra)
    {
      if(auto const* const reg = find_register(rcc, other); reg != nullptr and holds(*reg, name))
      {
        return enable_bit{.reg = reg->name, .field = name};
      }
    }
  }
  return std::nullopt;
}
}
