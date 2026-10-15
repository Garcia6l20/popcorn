#pragma once

#include <popcorn/export.hpp>

#include <string_view>

namespace popcorn::core
{
/** @brief Version string of popcorn and of the linked Unicorn commit. */
POPCORN_API auto version() -> std::string_view;
}
