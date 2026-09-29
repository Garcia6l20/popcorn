#pragma once

#include <popcorn/core/errors.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <span>
#include <string_view>
#include <vector>

namespace popcorn::periph::devices
{
/**
 * @brief FNV-1a digest of a device's initial contents, which a snapshot records so that it
 *        loads only into a device made from the same image.
 * @param bytes Contents.
 */
[[nodiscard]] inline auto digest_of(std::span<std::byte const> bytes) -> std::uint64_t
{
  constexpr std::uint64_t prime = 0x0000'0100'0000'01B3;
  auto                    hash  = std::uint64_t{0xCBF2'9CE4'8422'2325};
  for(auto const value : bytes)
  {
    hash = (hash ^ std::to_integer<std::uint64_t>(value)) * prime;
  }
  return hash;
}

/**
 * @brief Initial contents of a memory device: erased, all 0xFF, then the bytes of @p image.
 * @param image File to read, empty for an erased array.
 * @param size Array size in bytes.
 * @param device Device name for errors, such as `AT25SF`.
 * @throws config_error when the file cannot be read or is larger than @p size.
 */
[[nodiscard]] inline auto
    read_image(std::filesystem::path const& image, std::size_t size, std::string_view device)
        -> std::vector<std::byte>
{
  std::vector<std::byte> array(size, std::byte{0xFF});
  if(image.empty())
  {
    return array;
  }
  std::ifstream file{image, std::ios::binary};
  if(not file)
  {
    throw config_error(image.string(), 0, "image", std::format("a readable {} image file", device));
  }
  std::vector<char> const bytes{std::istreambuf_iterator<char>{file}, {}};
  if(bytes.size() > array.size())
  {
    throw config_error(
        image.string(), 0, "image",
        std::format("an {} image of at most {} bytes, not {}", device, array.size(), bytes.size()));
  }
  std::memcpy(array.data(), bytes.data(), bytes.size());
  return array;
}
}
