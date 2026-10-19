#pragma once

#include <popcorn/arch/arch.hpp>

#include <reflex/enum.hpp>

#include <unicorn/unicorn.h>

#include <array>
#include <cstddef>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>

namespace popcorn::arch::armv8m
{
/** @brief Core registers in snapshot order, before the floating-point ones. */
inline constexpr std::array<std::string_view, 23> core_register_names{
    "r0",      "r1",      "r2",        "r3",      "r4",  "r5",  "r6", "r7",
    "r8",      "r9",      "r10",       "r11",     "r12", "lr",  "pc", "xpsr",
    "primask", "basepri", "faultmask", "control", "msp", "psp", "sp"};

/** @brief Floating-point registers in snapshot order, for a core with an FPU. */
inline constexpr std::array<std::string_view, 33> fp_register_names{
    "s0",  "s1",  "s2",  "s3",  "s4",  "s5",  "s6",  "s7",  "s8",  "s9",  "s10",
    "s11", "s12", "s13", "s14", "s15", "s16", "s17", "s18", "s19", "s20", "s21",
    "s22", "s23", "s24", "s25", "s26", "s27", "s28", "s29", "s30", "s31", "fpscr"};

/** @brief Engine id of a register name, from the `uc_arm_reg` enumerators. */
consteval auto engine_register(std::string_view name) -> int
{
  std::string upper{"UC_ARM_REG_"};
  for(char const c : name)
  {
    upper.push_back(c >= 'a' and c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c);
  }
  return reflex::to_enum_value<uc_arm_reg>(upper).value();
}

/** @brief Registers a TrustZone machine appends: the security state, then the banked special
 * registers of both states. */
inline constexpr std::array<std::string_view, 17> trustzone_register_names{
    "secure",       "msp_s",     "msp_ns",     "psp_s",     "psp_ns",     "control_s",
    "control_ns",   "primask_s", "primask_ns", "basepri_s", "basepri_ns", "faultmask_s",
    "faultmask_ns", "msplim_s",  "msplim_ns",  "psplim_s",  "psplim_ns"};

/** @brief Register list made of @p lists in order, every register 32 bits. */
template <std::size_t N>
consteval auto register_list(std::initializer_list<std::span<std::string_view const>> lists)
    -> std::array<cpu_register, N>
{
  std::array<cpu_register, N> out{};
  std::size_t                 at = 0;
  for(auto const list : lists)
  {
    for(auto const name : list)
    {
      out[at++] = {.name = name, .engine_id = engine_register(name), .bits = 32};
    }
  }
  return out;
}

/** @brief Register list of a TrustZone core with an FPU: core, floating-point, then TrustZone. */
inline constexpr auto fpu_trustzone_registers = register_list<
    core_register_names.size() + fp_register_names.size() + trustzone_register_names.size()>(
    {core_register_names, fp_register_names, trustzone_register_names});

/** @brief Register list of a TrustZone core without an FPU: core, then TrustZone. */
inline constexpr auto integer_trustzone_registers =
    register_list<core_register_names.size() + trustzone_register_names.size()>(
        {core_register_names, trustzone_register_names});

/** @brief Register list of a core with an FPU: core registers then floating-point ones. */
inline constexpr auto fpu_registers = std::span<cpu_register const>{fpu_trustzone_registers}.first(
    core_register_names.size() + fp_register_names.size());

/** @brief Register list of a core without an FPU. */
inline constexpr auto integer_registers =
    std::span<cpu_register const>{integer_trustzone_registers}.first(core_register_names.size());
}
