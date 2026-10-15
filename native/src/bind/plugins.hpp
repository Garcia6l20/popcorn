#pragma once

#include <reflex/py.hpp>

namespace popcorn::bind::detail
{
/**
 * @brief Loads the built-in plugins armv8m and stm32 from popcorn core's directory, scope
 *        builtin, so their models and arch module register through the plugin loader.
 * @throws config_error from plugin::load, which fails the import of `_core`.
 */
void load_builtin_plugins();

/** @brief Binds the private plugin loader: `_load_plugin`, `_Plugin` and `_host_abi_tag`. */
void bind_plugins(reflex::py::nb::module_ scope);
}
