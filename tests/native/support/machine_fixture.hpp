#pragma once

#include <popcorn/core/machine.hpp>

#include "elf_image.hpp"
#include "firmware.hpp"
#include "test_arch.hpp"

#include <doctest/doctest.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace popcorn::testing
{
/**
 * @brief Description named by an environment variable the build script sets.
 * @param variable Variable holding the path, such as `POPCORN_M33_DESC`.
 */
inline auto description_from(char const* variable) -> mcu_description
{
  char const* const path = std::getenv(variable);
  REQUIRE_MESSAGE(path != nullptr, variable);
  return load_description(path);
}

/** @brief The core test description, `tests/data/core/m33.json`. */
inline auto m33() -> mcu_description
{
  return description_from("POPCORN_M33_DESC");
}

/** @brief The core test description with an rx alias over rw SRAM. */
inline auto m33_code_alias() -> mcu_description
{
  return description_from("POPCORN_M33_CODE_ALIAS_DESC");
}

/**
 * @brief Test firmware image, read once per process.
 * @param name Firmware name, such as `run`.
 */
inline auto elf(std::string_view name) -> image::image const&
{
  static std::map<std::string, image::image, std::less<>> cache;
  auto                                                    found = cache.find(name);
  if(found == cache.end())
  {
    found = cache.emplace(std::string{name}, read_elf(firmware(name))).first;
  }
  return found->second;
}

/**
 * @brief Address of a symbol of a test firmware.
 * @param firmware_name Firmware name.
 * @param name Symbol name.
 */
inline auto sym(std::string_view firmware_name, std::string_view name) -> address
{
  return find_symbol(elf(firmware_name), name);
}

/**
 * @brief Machine over a test arch, a firmware loaded and reset.
 * @tparam Arch Test arch type, test_arch or a type derived from it.
 */
template <typename Arch = test_arch> struct machine_fixture
{
  /** @brief The arch module the machine created. */
  Arch* module = nullptr;
  /** @brief The machine. */
  std::unique_ptr<core::machine> target;

  /**
   * @brief Builds, loads and resets.
   * @param desc Description.
   * @param firmware_name Firmware to load.
   * @param options Machine options.
   * @param extra Extra arguments of the Arch constructor after the core.
   */
  machine_fixture(
      mcu_description       desc,
      std::string_view      firmware_name,
      core::machine_options options,
      auto... extra)
  {
    target = std::make_unique<core::machine>(
        std::move(desc),
        [this, extra...](core_desc const& core) -> std::unique_ptr<arch::arch> {
          auto made = std::make_unique<Arch>(core, extra...);
          module    = made.get();
          return made;
        },
        std::move(options));
    target->load(elf(firmware_name));
    target->reset();
  }

  /** @brief Reads a register by name. */
  auto reg(std::string_view name) const -> std::uint64_t
  {
    return target->regs().read(*target->regs().find(name));
  }

  /** @brief Writes a register by name. */
  void set(std::string_view name, std::uint64_t value) const
  {
    target->regs().write(*target->regs().find(name), value);
  }

  /** @brief Runs one slice. */
  auto run(core::slice_request const& request = {}) const -> core::slice_result
  {
    return target->run_slice(request);
  }

  /** @brief Runs one slice with a 10 s wall budget and requires a stop. */
  auto run_to_stop(core::slice_request request = {}) const -> stop
  {
    request.wall_budget = std::chrono::seconds{10};
    auto const result   = run(request);
    REQUIRE(result.stopped.has_value());
    return *result.stopped;
  }

  /** @brief Reads a guest word. */
  auto word(address at) const -> std::uint32_t
  {
    auto const value = target->mem().read(at, 4);
    REQUIRE(value.has_value());
    return *value;
  }
};
}
