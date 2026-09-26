#pragma once

#include "../support/machine_fixture.hpp"

#include <popcorn/arch/armv8m/armv8m.hpp>
#include <popcorn/core/machine.hpp>
#include <popcorn/periph/stm32/builtin.hpp>

#include <doctest/doctest.h>

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
inline auto u575() -> mcu_description const&
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
      mcu_description       desc,
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
}
