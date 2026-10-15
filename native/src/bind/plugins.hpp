#pragma once

#include <reflex/py.hpp>

namespace popcorn::bind::detail
{
/** @brief Binds the private plugin loader: `_load_plugin`, `_Plugin` and `_host_abi_tag`. */
void bind_plugins(reflex::py::nb::module_ scope);
}
