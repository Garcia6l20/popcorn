#pragma once

#include "row_of.hpp"

#include <array>
#include <span>
#include <string_view>

namespace popcorn::periph::stm32
{
/** @brief A TZSC `<STEM>SEC` or TZIC `<STEM>IE` stem whose peripheral has another name. */
struct gtzc_rename
{
  /** @brief Stem of the field. */
  std::string_view stem;
  /** @brief Peripheral the stem names. */
  std::string_view peripheral;
};

/** @brief A peripheral that one TZSC or TZIC bit covers together with another. */
struct gtzc_companion
{
  /** @brief Peripheral the stem names, after the renames. */
  std::string_view primary;
  /** @brief Peripheral the same bit also covers. */
  std::string_view also;
};

/** @brief Differences between the GTZC TZSC and TZIC field stems of the families a model row
 *         serves: the stems that name no peripheral of the same name. */
struct gtzc_variant
{
  /** @brief Stems naming a peripheral of another name. */
  std::span<gtzc_rename const> renames;
  /** @brief Peripherals one bit covers with the peripheral it names. */
  std::span<gtzc_companion const> companions;
};

inline constexpr auto u5_gtzc_renames    = std::to_array<gtzc_rename>({
    {"USART4",       "UART4"   },
    {"ICACHE",       "ICache"  },
    {"ICACHE_REG",   "ICache"  },
    {"DCACHE_REG",   "DCACHE"  },
    {"OTGFS",        "OTG_FS"  },
    {"FSMC",         "FMC"     },
    {"FSMC_REG",     "FMC"     },
    {"OCTOSPI1_REG", "OCTOSPI1"},
    {"OCTOSPI2_REG", "OCTOSPI2"},
    {"FLASH_REG",    "FLASH"   },
});
inline constexpr auto u5_gtzc_companions = std::to_array<gtzc_companion>({
    {"DCMI",     "PSSI"      },
    {"ADC1",     "ADC12"     },
    {"FDCAN1",   "FDCAN1_RAM"},
    {"SDMMC1",   "DLYBSD1"   },
    {"SDMMC2",   "DLYBSD2"   },
    {"OCTOSPI1", "DLYBOS1"   },
    {"OCTOSPI2", "DLYBOS2"   },
});
/** @brief STM32U5 GTZC1 and GTZC2, RM0456 5.7.4 to 5.7.6 and 5.8: the SVD instance names of
 *         UART4, the instruction cache, the OTG, the FMC and the register blocks, the bits
 *         that RM0456 Table 5 gives to two blocks, and each delay block with the peripheral
 *         RM0456 Table 297 associates it with. */
inline constexpr gtzc_variant u5_gtzc{u5_gtzc_renames, u5_gtzc_companions};

inline constexpr auto h5_gtzc_renames    = std::to_array<gtzc_rename>({
    {"ADC12",     "ADC1"   },
    {"DAC1",      "DAC"    },
    {"I3C1",      "I3C"    },
    {"LPUART1",   "LPUART" },
    {"OCTOSPI1",  "OCTOSPI"},
    {"UCPD",      "UCPD1"  },
    {"FLASH_REG", "FLASH"  },
});
inline constexpr auto h5_gtzc_companions = std::to_array<gtzc_companion>({
    {"DCMI",    "PSSI"   },
    {"ADC1",    "ADC2"   },
    {"ADC1",    "ADCC"   },
    {"SDMMC1",  "DLYBSD1"},
    {"SDMMC2",  "DLYBSD2"},
    {"OCTOSPI", "DLYBOS1"},
});
/** @brief STM32H562, H563 and H573 GTZC1, RM0481 5.7.4 to 5.7.6 and 5.8: the SVD names
 *         `DAC`, `I3C`, `LPUART`, `OCTOSPI` and `UCPD1` of the stems `DAC1`, `I3C1`,
 *         `LPUART1`, `OCTOSPI1` and `UCPD`, and `ADC12SEC` covering ADC1, ADC2 and their common
 *         registers `ADCC`, RM0481 Table 3, and each delay block with the peripheral RM0481
 *         Table 275 associates it with. `HDMICEC` names no peripheral of the SVD. */
inline constexpr gtzc_variant h5_gtzc{h5_gtzc_renames, h5_gtzc_companions};

/**
 * @brief Factory of the GTZC TZSC, TZIC and MPCBB models, RM0456 and RM0481 section 5. Returns
 *        no model without TrustZone: the GTZC registers then stay storage at their description
 *        reset values.
 * @param variant Field stems of the family the row serves.
 */
[[nodiscard]] auto gtzc_factory(gtzc_variant variant) -> model_factory;

/** @brief The GTZC model rows. */
[[nodiscard]] auto gtzc_rows() -> std::span<builtin const>;
}
