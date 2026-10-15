#pragma once

#include <popcorn/export.hpp>
#include <popcorn/image/image.hpp>
#include <popcorn/obs/coverage.hpp>

#include <string_view>

namespace popcorn::obs
{
/**
 * @brief Reference of a loaded image: kind, path made absolute when not empty, digest and the
 *        extent of its executable segments.
 * @param loaded Image.
 * @param kind What it was loaded from.
 * @param path Its file, empty for a raw image.
 */
POPCORN_API auto reference_of(image::image const& loaded, image_kind kind, std::string_view path)
    -> image_ref;
}
