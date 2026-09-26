#pragma once

#include <popcorn/config/project.hpp>

#include <reflex/py.hpp>

#include <optional>
#include <string>

namespace popcorn::bind
{
/**
 * @brief Reads a project file with a config applied.
 * @param path Path of the project file.
 * @param config Config to apply, empty for none.
 * @throws config_error as config::load_project().
 */
[[= reflex::py::doc{"Read a project file, applying the named config."}]] auto
    load_project(std::string const& path, std::optional<std::string> const& config)
        -> config::loaded_project;
}
