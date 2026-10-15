#pragma once

/**
 * @file
 * @brief Included by a plugin with Python bindings. It fails the build when the plugin's
 *        nanobind is not the version popcorn was built with, the version the `;nanobind`
 *        field of the ABI tag states.
 */

#include <popcorn/plugin/abi.hpp>

#include <nanobind/nanobind.h>

static_assert(
    NB_VERSION_MAJOR == POPCORN_NANOBIND_VERSION_MAJOR
        and NB_VERSION_MINOR == POPCORN_NANOBIND_VERSION_MINOR
        and NB_VERSION_PATCH == POPCORN_NANOBIND_VERSION_PATCH,
    "a popcorn plugin builds with the nanobind version popcorn was built with");
