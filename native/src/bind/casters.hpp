/**
 * @file
 * @brief The nanobind type casters every binding translation unit using nanobind shares, through
 *        a header each of them includes. A unit lacking one falls back to nanobind's generic
 *        caster for that type: an ODR violation, and a call that converts no argument and throws
 *        nanobind::cast_error.
 */
#pragma once

#include <nanobind/stl/string.h>
