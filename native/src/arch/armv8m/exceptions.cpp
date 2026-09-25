#include "exceptions.hpp"

#include <popcorn/core/errors.hpp>

#include <unicorn/unicorn.h>

#include <format>
#include <string_view>

namespace popcorn::arch::armv8m
{
namespace
{
constexpr std::uint32_t control_npriv = 1u << 0;
constexpr std::uint32_t xpsr_ge       = 0x000f'0000;

void write(uc_struct* engine, uc_arm_reg id, std::uint32_t value, std::string_view what)
{
  if(auto const err = uc_reg_write(engine, id, &value); err != UC_ERR_OK)
  {
    throw emulation_error(std::format("cortex_m writing {}: {}", what, uc_strerror(err)));
  }
}

auto read(uc_struct* engine, uc_arm_reg id, std::string_view what) -> std::uint32_t
{
  std::uint32_t value = 0;
  if(auto const err = uc_reg_read(engine, id, &value); err != UC_ERR_OK)
  {
    throw emulation_error(std::format("cortex_m reading {}: {}", what, uc_strerror(err)));
  }
  return value;
}
}

void set_context(uc_struct* engine, special_registers const& target)
{
  write(engine, UC_ARM_REG_IPSR, 1, "IPSR");
  write(
      engine, UC_ARM_REG_CONTROL, read(engine, UC_ARM_REG_CONTROL, "CONTROL") & ~control_npriv,
      "CONTROL");
  write(engine, UC_ARM_REG_IPSR, 0, "IPSR");
  write(engine, UC_ARM_REG_CONTROL, target.control & ~control_npriv, "CONTROL");
  write(engine, UC_ARM_REG_MSP, target.msp, "MSP");
  write(engine, UC_ARM_REG_PSP, target.psp, "PSP");
  write(engine, UC_ARM_REG_PRIMASK, target.primask, "PRIMASK");
  write(engine, UC_ARM_REG_BASEPRI, target.basepri, "BASEPRI");
  write(engine, UC_ARM_REG_FAULTMASK, target.faultmask, "FAULTMASK");
  write(engine, UC_ARM_REG_IPSR, target.ipsr, "IPSR");
  write(engine, UC_ARM_REG_CONTROL, target.control, "CONTROL");
  write(engine, UC_ARM_REG_APSR_G, target.ge, "APSR.GE");
}

auto read_special(uc_struct* engine) -> special_registers
{
  auto const control      = read(engine, UC_ARM_REG_CONTROL, "CONTROL");
  auto const ipsr         = read(engine, UC_ARM_REG_IPSR, "IPSR");
  auto const unprivileged = ipsr == 0 and (control & control_npriv) != 0;
  if(unprivileged)
  {
    write(engine, UC_ARM_REG_IPSR, 1, "IPSR");
  }
  special_registers const out{
      .control   = control,
      .primask   = read(engine, UC_ARM_REG_PRIMASK, "PRIMASK"),
      .basepri   = read(engine, UC_ARM_REG_BASEPRI, "BASEPRI"),
      .faultmask = read(engine, UC_ARM_REG_FAULTMASK, "FAULTMASK"),
      .msp       = read(engine, UC_ARM_REG_MSP, "MSP"),
      .psp       = read(engine, UC_ARM_REG_PSP, "PSP"),
      .ipsr      = ipsr,
      .ge        = read(engine, UC_ARM_REG_XPSR, "xPSR") & xpsr_ge,
  };
  if(unprivileged)
  {
    write(engine, UC_ARM_REG_IPSR, 0, "IPSR");
  }
  return out;
}
}
