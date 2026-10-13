#pragma once

#include "../support/machine_fixture.hpp"

#include <popcorn/arch/armv8m/armv8m.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/periph/stm32/builtin.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace popcorn::testing
{
/** @brief The STM32U575 description built from ST's files, `POPCORN_U575_DESC`, read once. */
inline auto u575() -> part_description const&
{
  static auto const desc = description_from("POPCORN_U575_DESC");
  return desc;
}

/** @brief Machine options of the STM32 tests: faults and spins stop the run. */
inline auto stm32_options() -> core::machine_options
{
  core::machine_options options;
  options.faults         = arch::fault_policy::stop;
  options.spin_threshold = 1'000'000;
  return options;
}

/** @brief Machine over the Cortex-M module and the built-in STM32 models, a firmware loaded. */
struct stm32_fixture
{
  /** @brief Firmware loaded. */
  std::string firmware_name;
  /** @brief The machine. */
  std::unique_ptr<core::machine> target;

  /**
   * @brief Registers the built-in models, builds the machine, loads and resets.
   * @param desc Description.
   * @param name Firmware to load.
   * @param options Machine options.
   */
  stm32_fixture(
      part_description      desc,
      std::string_view      name,
      core::machine_options options = stm32_options())
      : firmware_name{name}
  {
    periph::stm32::register_builtin_models();
    target = std::make_unique<core::machine>(
        std::move(desc), arch::armv8m::factory(), std::move(options));
    target->load(elf(firmware_name));
    target->reset();
  }

  /** @brief Writes a core register by name. */
  void set(std::string_view name, std::uint64_t value) const
  {
    target->regs().write(*target->regs().find(name), value);
  }

  /**
   * @brief Value of a register field, resolved by name from the description.
   * @param peripheral Instance name.
   * @param reg Register name.
   * @param field Field name.
   */
  [[nodiscard]] auto
      field(std::string_view peripheral, std::string_view reg, std::string_view field) const
      -> std::uint32_t
  {
    auto const& regs  = target->peripheral(peripheral);
    auto const  found = std::ranges::find(regs.description().registers, reg, &register_desc::name);
    REQUIRE(found != regs.description().registers.end());
    auto const bits = std::ranges::find(found->fields, field, &field_desc::name);
    REQUIRE(bits != found->fields.end());
    auto const mask = bits->width >= 32 ? ~std::uint32_t{} : (std::uint32_t{1} << bits->width) - 1u;
    return (regs.peek(found->offset & ~3u) >> (8 * (found->offset % 4) + bits->lsb)) & mask;
  }

  /** @brief Runs one slice. */
  auto run(core::slice_request const& request) const -> core::slice_result
  {
    return target->run_slice(request);
  }

  /** @brief Runs one slice with a 60 s wall budget and requires a stop. */
  auto run_to_stop(core::slice_request request = {.max_instructions = icount{100'000'000}}) const
      -> stop
  {
    request.wall_budget = std::chrono::seconds{60};
    auto const result   = target->run_slice(request);
    REQUIRE(result.stopped.has_value());
    return *result.stopped;
  }

  /** @brief The firmware's `log_buf` words up to `log_len`. */
  [[nodiscard]] auto log() const -> std::vector<std::uint32_t>
  {
    auto const                 count = read(sym(firmware_name, "log_len"));
    auto const                 base  = sym(firmware_name, "log_buf");
    std::vector<std::uint32_t> out;
    for(std::uint32_t index = 0; index < count and index < 256; ++index)
    {
      out.push_back(read(base + 4 * index));
    }
    return out;
  }

  /**
   * @brief Model of a peripheral as the type a test expects.
   * @tparam T Model type.
   * @param name Peripheral instance name.
   * @return The model, null when none serves the peripheral or it is not a @p T.
   */
  template <typename T> [[nodiscard]] auto model_of(std::string_view name) const -> T*
  {
    return dynamic_cast<T*>(target->model(name));
  }

private:
  [[nodiscard]] auto read(address at) const -> std::uint32_t
  {
    auto const value = target->mem().read(at, 4);
    REQUIRE(value.has_value());
    return static_cast<std::uint32_t>(*value);
  }
};

/**
 * @brief Sets or clears a DBGMCU freeze field, found by its SVD name in any DBGMCU register.
 * @param f Machine to write.
 * @param name Field name, such as `DBG_IWDG_STOP`.
 * @param on Value of the bit.
 */
inline void set_debug_freeze(stm32_fixture const& f, std::string_view name, bool on)
{
  auto const& desc = f.target->peripheral("DBGMCU").description();
  for(auto const& reg : desc.registers)
  {
    auto const field = std::ranges::find(reg.fields, name, &field_desc::name);
    if(field != reg.fields.end())
    {
      auto const at  = desc.base + reg.offset;
      auto const old = f.target->mem().read(at, 4);
      REQUIRE(old.has_value());
      auto const bit = std::uint32_t{1} << field->lsb;
      REQUIRE(f.target->mem().write(at, 4, on ? (*old | bit) : (*old & ~bit)).has_value());
      return;
    }
  }
  FAIL("no DBGMCU field " << name);
}

/**
 * @brief Whether a DBGMCU freeze field is set.
 * @param f Machine to read.
 * @param name Field name, such as `DBG_IWDG_STOP`.
 */
inline auto debug_freeze_set(stm32_fixture const& f, std::string_view name) -> bool
{
  auto const& desc = f.target->peripheral("DBGMCU").description();
  for(auto const& reg : desc.registers)
  {
    auto const field = std::ranges::find(reg.fields, name, &field_desc::name);
    if(field != reg.fields.end())
    {
      auto const value = f.target->mem().read(desc.base + reg.offset, 4);
      REQUIRE(value.has_value());
      return (*value >> field->lsb & 1u) != 0;
    }
  }
  FAIL("no DBGMCU field " << name);
  return false;
}
}
