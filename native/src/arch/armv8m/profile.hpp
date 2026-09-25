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
};

/** @brief Every Cortex-M core the module knows, enabled or not. */
inline constexpr std::array profiles{
    profile{"CM33",    architecture::v8m_main, 0xd21, UC_CPU_ARM_CORTEX_M33, true,  true,  true },
    profile{"CM4",     architecture::v7m,      0xc24, UC_CPU_ARM_CORTEX_M4,  false, false, false},
    profile{"CM7",     architecture::v7m,      0xc27, UC_CPU_ARM_CORTEX_M7,  false, false, false},
    profile{"CM3",     architecture::v7m,      0xc23, UC_CPU_ARM_CORTEX_M3,  false, false, false},
    profile{"CM0",     architecture::v6m,      0xc20, UC_CPU_ARM_CORTEX_M0,  false, false, false},
    profile{"CM0PLUS", architecture::v6m,      0xc60, UC_CPU_ARM_CORTEX_M0,  false, false, false},
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
