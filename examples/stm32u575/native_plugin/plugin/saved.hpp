#pragma once

#include <popcorn/core/errors.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace native_plugin
{
/**
 * @brief Appends the bytes of @p value to @p out, one field of a model's saved state.
 * @param out Bytes of the snapshot being written.
 * @param value Field to save.
 */
template <typename T> void append(std::vector<std::byte>& out, T value)
{
  auto const bytes = std::bit_cast<std::array<std::byte, sizeof(T)>>(value);
  out.insert(out.end(), bytes.begin(), bytes.end());
}

/**
 * @brief Reads the next field of a model's saved state from @p in, then drops it from @p in.
 * @param in Bytes of the snapshot not read yet, at least `sizeof(T)`.
 * @return The field.
 */
template <typename T> auto take(std::span<std::byte const>& in) -> T
{
  std::array<std::byte, sizeof(T)> bytes{};
  std::copy_n(in.begin(), sizeof(T), bytes.begin());
  in = in.subspan(sizeof(T));
  return std::bit_cast<T>(bytes);
}

/**
 * @brief Refuses saved state of another size than @p size.
 * @param owner Name of the model or device, as the error reports it.
 * @param in Bytes of one save() call.
 * @param size Bytes its save() writes.
 * @throws popcorn::config_error when @p in holds another number of bytes.
 */
inline void expect_size(std::string const& owner, std::span<std::byte const> in, std::size_t size)
{
  if(in.size() != size)
  {
    throw popcorn::config_error(
        owner, 0, "snapshot",
        "a snapshot of " + std::to_string(size) + " bytes, not " + std::to_string(in.size()));
  }
}
}
