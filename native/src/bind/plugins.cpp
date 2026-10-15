#include "plugins.hpp"

#include <popcorn/plugin/bindings.hpp>
#include <popcorn/plugin/plugin.hpp>

#include <nanobind/nanobind.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/vector.h>

#include <ranges>
#include <string>
#include <vector>

namespace popcorn::bind::detail
{
namespace nb = reflex::py::nb;

void load_builtin_plugins()
{
  auto const directory = plugin::library_directory();
  for(auto const* const name : {"libpopcorn_armv8m.so", "libpopcorn_stm32.so"})
  {
    static_cast<void>(plugin::load(directory / name, plugin::scope::builtin));
  }
}

void bind_plugins(nb::module_ scope)
{
  nb::class_<plugin::loaded>(scope, "_Plugin", "A loaded native plugin, never unloaded.")
      .def_prop_ro(
          "name", [](plugin::loaded const& self) { return self.name; },
          "The name POPCORN_PLUGIN gives it.")
      .def_prop_ro(
          "path", [](plugin::loaded const& self) { return self.path.string(); },
          "Canonical path of the library.")
      .def_prop_ro(
          "abi_tag", [](plugin::loaded const& self) { return self.abi_tag; },
          "The ABI tag it embeds.")
      .def_prop_ro(
          "models",
          [](plugin::loaded const& self) {
            return self.models
                 | std::views::transform(&plugin::model_entry::name)
                 | std::ranges::to<std::vector<std::string>>();
          },
          "Names of the models its init recorded, in order.");
  scope.def(
      "_load_plugin",
      [](std::string const& path) -> plugin::loaded const& {
        return plugin::load(path, plugin::scope::declared);
      },
      nb::arg("path"), nb::rv_policy::reference,
      "Load the native plugin at path as a declared plugin, once per library file. ConfigError "
      "when it was built for another popcorn, is not a plugin, or registers globally.");
  scope.def(
      "_registry_anchor", [] { return plugin::registry_anchor(); },
      "Address of popcorn core's registry as _core links it, compared by plugin::require_core.");
  scope.def(
      "_host_abi_tag", [] { return std::string{plugin::host_abi_tag()}; },
      "The ABI tag every plugin must embed.");
}
}
