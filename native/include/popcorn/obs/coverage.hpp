#pragma once

#include <popcorn/core/types.hpp>
#include <popcorn/image/image.hpp>

#include <span>
#include <string>
#include <vector>

namespace popcorn::obs
{
/**
 * @brief Code ranges of functions by name, for scoping coverage and trace sinks.
 * @param images Images to search, in load order. The first image holding a name wins.
 * @param names Function names.
 * @return One range per name, in the order of @p names: the symbol's value and size, named after
 *         it. A symbol of size 0 reaches the next function symbol inside the executable segment
 *         holding it, or that segment's end. Held by no executable segment, it reaches the next
 *         function symbol of its image.
 * @throws symbol_error when no image has a name, as image::image::lookup() reports it for the
 *         last image searched, when the first image having it holds it ambiguously, as lookup()
 *         reports it, or for a size-0 symbol held by no executable segment and followed by no
 *         function symbol.
 */
auto function_ranges(std::span<image::image const> images, std::span<std::string const> names)
    -> std::vector<address_range>;
}
