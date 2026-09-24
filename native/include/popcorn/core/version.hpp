#pragma once

#include <string_view>

namespace popcorn::core
{
/** @brief Version string of popcorn and of the linked Unicorn commit. */
auto version() -> std::string_view;
}
