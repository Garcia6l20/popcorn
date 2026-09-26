#include "config.hpp"

#include <nanobind/nanobind.h>

namespace popcorn::bind
{
auto load_project(std::string const& path, std::optional<std::string> const& config)
    -> config::loaded_project
{
  reflex::py::nb::gil_scoped_release unlocked;
  return config::load_project(path, config);
}
}
