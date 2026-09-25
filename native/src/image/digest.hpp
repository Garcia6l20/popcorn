#pragma once

#include <popcorn/image/image.hpp>

#include <string>
#include <vector>

namespace popcorn::image::detail
{
/**
 * @brief SHA-256 digest of an image's loadable content, lower-case hex.
 * @param segments Segments in address order, as load_elf, load_bin and load_hex produce them.
 * @return The digest of the coalesced address-to-bytes map the segments produce: adjoining
 *         segments merge into one run before hashing, so a different segment split for the same
 *         bytes at the same addresses gives the same digest.
 */
auto digest_of(std::vector<segment> const& segments) -> std::string;
}
