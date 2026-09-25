#pragma once

#include <popcorn/arch/arch.hpp>

#include <reflex/enum.hpp>

#include <unicorn/unicorn.h>

#include <array>
#include <cstddef>
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

/** @brief Register list of a core with an FPU: core registers then floating-point ones. */
inline constexpr auto fpu_registers = [] consteval {
  std::array<cpu_register, core_register_names.size() + fp_register_names.size()> out{};
  std::size_t                                                                     at = 0;
  for(auto const name : core_register_names)
  {
    out[at++] = {.name = name, .engine_id = engine_register(name), .bits = 32};
  }
  for(auto const name : fp_register_names)
  {
    out[at++] = {.name = name, .engine_id = engine_register(name), .bits = 32};
  }
  return out;
}();

/** @brief Register list of a core without an FPU. */
inline constexpr auto integer_registers =
    std::span<cpu_register const>{fpu_registers}.first(core_register_names.size());
}
