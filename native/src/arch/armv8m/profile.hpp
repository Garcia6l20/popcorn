#pragma once

#include <unicorn/unicorn.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

namespace popcorn::arch::armv8m
{
/** @brief M-profile architecture of a core. */
enum class architecture
{
  v6m,
  v7m,
  v8m_main,
};

/** @brief Fixed facts of one Cortex-M core, as the description's `core.cpu` names it. */
struct profile
{
  /** @brief Core name as SVD spells it. */
  std::string_view cpu;
  /** @brief Architecture. */
  architecture arch;
  /** @brief CPUID PARTNO. */
  std::uint16_t cpuid_part;
  /** @brief Unicorn CPU model. */
  uc_cpu_arm engine_model;
  /** @brief Whether the engine opens with `UC_MODE_MCLASS`, which forces a Cortex-M33. */
  bool mclass_mode;
  /** @brief Whether the engine runs the core in the Secure state. */
  bool secure_engine;
  /** @brief Whether this release implements the core. */
  bool enabled;
  /** @brief Whether the FPU is FPv5, which sets MVFR2, else FPv4. */
  bool fpv5;
  /** @brief Whether CCR.IC, CCR.DC and CCR.BP exist, the Cortex-M7 caches. */
  bool caches;
  /** @brief ID_PFR0 to ID_ISAR5, CLIDR and CTR, 0xE000ED40-0xE000ED7C, from Unicorn's model. */
  std::array<std::uint32_t, 16> ids;
};

/** @brief ID registers of the ARMv7-M Cortex-M3 and Cortex-M4 models. */
inline constexpr std::array<std::uint32_t, 16> v7m_ids{
    0x0000'0030, 0x0000'0200, 0x0010'0000, 0x0000'0000, 0x0000'0030, 0x0000'0000,
    0x0000'0000, 0x0000'0000, 0x0114'1110, 0x0211'1000, 0x2111'2231, 0x0111'1110,
    0x0131'0102, 0x0000'0000, 0x0000'0000, 0x0000'0000};

/** @brief ID registers of the Cortex-M7 model. */
inline constexpr std::array<std::uint32_t, 16> m7_ids{
    0x0000'0030, 0x0000'0200, 0x0010'0000, 0x0000'0000, 0x0010'0030, 0x0000'0000,
    0x0100'0000, 0x0000'0000, 0x0110'1110, 0x0211'2000, 0x2023'2231, 0x0111'1131,
    0x0131'0132, 0x0000'0000, 0x0000'0000, 0x0000'0000};

/** @brief ID registers of the Cortex-M33 model. */
inline constexpr std::array<std::uint32_t, 16> m33_ids{
    0x0000'0030, 0x0000'0210, 0x0020'0000, 0x0000'0000, 0x0010'1f40, 0x0000'0000,
    0x0100'0000, 0x0000'0000, 0x0110'1110, 0x0221'2000, 0x2023'2232, 0x0111'1131,
    0x0131'0132, 0x0000'0000, 0x0000'0000, 0x8000'c000};

/** @brief Every Cortex-M core the module knows, enabled or not. */
inline constexpr std::array profiles{
    profile{
            "CM33",    architecture::v8m_main, 0xd21, UC_CPU_ARM_CORTEX_M33, true,  true,  true,  true,  false,
            m33_ids                                                                                                    },
    profile{
            "CM4",     architecture::v7m,      0xc24, UC_CPU_ARM_CORTEX_M4,  false, false, true,  false, false,
            v7m_ids                                                                                                    },
    profile{
            "CM7",     architecture::v7m,      0xc27, UC_CPU_ARM_CORTEX_M7,  false, false, true,  true,  true,
            m7_ids                                                                                                     },
    profile{
            "CM3",     architecture::v7m,      0xc23, UC_CPU_ARM_CORTEX_M3,  false, false, true,  false, false,
            v7m_ids                                                                                                    },
    profile{
            "CM0",     architecture::v6m,      0xc20, UC_CPU_ARM_CORTEX_M0,  false, false, false, false, false,
            v7m_ids                                                                                                    },
    profile{
            "CM0PLUS", architecture::v6m,      0xc60, UC_CPU_ARM_CORTEX_M0,  false, false, false, false,
            false,                                                                                              v7m_ids},
};

/** @brief Enabled profile of a core name, empty when the module does not implement it. */
constexpr auto find_profile(std::string_view cpu) noexcept -> std::optional<profile>
{
  auto const found = std::ranges::find(profiles, cpu, &profile::cpu);
  if(found == profiles.end() or not found->enabled)
  {
    return std::nullopt;
  }
  return *found;
}
}
