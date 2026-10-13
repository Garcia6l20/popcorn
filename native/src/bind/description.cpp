#include "description.hpp"

#include <nanobind/nanobind.h>

namespace popcorn::bind
{
auto load_description(std::string const& path) -> part_description
{
  reflex::py::nb::gil_scoped_release unlocked;
  return popcorn::load_description(path);
}

auto parse_description(std::string const& json, std::string const& source) -> part_description
{
  reflex::py::nb::gil_scoped_release unlocked;
  return popcorn::parse_description(json, source);
}

auto dump_description(part_description const& description) -> std::string
{
  return popcorn::dump_description(description);
}

auto description_digest(part_description const& description) -> std::string
{
  return popcorn::description_digest(description);
}
}
