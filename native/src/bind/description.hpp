#pragma once

#include "casters.hpp"

#include <popcorn/core/description.hpp>

#include <reflex/py.hpp>

#include <string>

namespace popcorn::bind
{
/**
 * @brief Reads an MCU description JSON file.
 * @param path Path of the file.
 * @throws config_error when the file cannot be read or does not match the schema.
 */
[[= reflex::py::doc{"Read an MCU description JSON file."}]] auto
    load_description(std::string const& path) -> mcu_description;

/**
 * @brief Parses MCU description JSON text.
 * @param json The text.
 * @param source Name of the text in error messages.
 * @throws config_error when the text does not match the schema.
 */
[[= reflex::py::doc{"Parse MCU description JSON text, source naming it in errors."}]] auto
    parse_description(std::string const& json, std::string const& source) -> mcu_description;

/**
 * @brief Serializes a description to the JSON parse_description() reads.
 * @param description The description.
 */
[[= reflex::py::doc{"JSON text of a description."}]] auto
    dump_description(mcu_description const& description) -> std::string;

/**
 * @brief Digest a snapshot records for a description.
 * @param description The description.
 */
[[= reflex::py::doc{"SHA-256 of the canonical JSON of a description, lower-case hex."}]] auto
    description_digest(mcu_description const& description) -> std::string;
}
