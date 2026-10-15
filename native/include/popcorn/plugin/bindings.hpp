#pragma once

/**
 * @file
 * @brief Included by a plugin with Python bindings. It fails the build when the plugin's
 *        nanobind is not the version popcorn was built with, the version the `;nanobind`
 *        field of the ABI tag states, or when the unit's `NB_DOMAIN` is not the domain popcorn's
 *        modules build in. A bindings module calls popcorn::plugin::require_core first, so a
 *        module that would read another copy of popcorn core, or another nanobind type
 *        registry, fails its import instead of running or aborting.
 */

#include <popcorn/periph/model.hpp>
#include <popcorn/periph/spi.hpp>
#include <popcorn/plugin/abi.hpp>

#include <nanobind/nanobind.h>
#include <nanobind/stl/string.h>

#include <cstdint>
#include <format>
#include <string>
#include <string_view>

static_assert(
    NB_VERSION_MAJOR == POPCORN_NANOBIND_VERSION_MAJOR
        and NB_VERSION_MINOR == POPCORN_NANOBIND_VERSION_MINOR
        and NB_VERSION_PATCH == POPCORN_NANOBIND_VERSION_PATCH,
    "a popcorn plugin builds with the nanobind version popcorn was built with");

#if defined(NB_DOMAIN)
static_assert(
    std::string_view{NB_DOMAIN_STR} == POPCORN_NB_DOMAIN,
    "a popcorn plugin builds in the nanobind domain popcorn was built in");
#else
static_assert(
    std::string_view{POPCORN_NB_DOMAIN}.empty(),
    "a popcorn plugin builds in the nanobind domain popcorn was built in");
#endif

namespace popcorn::plugin
{
/**
 * @brief The address of periph::registered_glob as the calling module links it. Every module
 *        linking the one libpopcorn_core.so of the process gets the same value, a module with
 *        its own copy of core gets another.
 */
[[nodiscard]] inline auto registry_anchor() noexcept -> std::uintptr_t
{
  return reinterpret_cast<std::uintptr_t>(&periph::registered_glob);
}

/**
 * @brief Imports `popcorn._core` and checks the calling module shares its core and its nanobind
 *        types. Call it first in `NB_MODULE`, before any `nb::class_`.
 * @param self The module being initialized, named in the errors.
 * @return The `popcorn._core` module.
 * @throws nanobind::import_error when the module's registry_anchor differs from
 *         `_core._registry_anchor()`, a second copy of popcorn core, or when its nanobind does
 *         not know periph::spi_target, another nanobind domain or internals version.
 */
inline auto require_core(nanobind::module_ const& self) -> nanobind::module_
{
  auto       core      = nanobind::module_::import_("popcorn._core");
  auto const name      = nanobind::cast<std::string>(self.attr("__name__"));
  auto const published = nanobind::cast<std::uintptr_t>(core.attr("_registry_anchor")());
  if(published != registry_anchor())
  {
    throw nanobind::import_error(
        std::format(
            "{} links its own copy of popcorn core, registry at {:#x}, popcorn._core's at "
            "{:#x}: a popcorn plugin module links libpopcorn_core.so",
            name, registry_anchor(), published)
            .c_str());
  }
  if(not nanobind::type<periph::spi_target>().is_valid())
  {
    throw nanobind::import_error(
        std::format(
            "{} does not share popcorn._core's nanobind types, popcorn::periph::spi_target is "
            "unknown to it: a popcorn plugin module builds with popcorn's nanobind and domain",
            name)
            .c_str());
  }
  return core;
}
}
