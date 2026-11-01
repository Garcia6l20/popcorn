#pragma once

#include <popcorn/core/errors.hpp>

#include <atomic>
#include <memory>
#include <string>
#include <string_view>

namespace popcorn::bind::detail
{
/** @brief Whether what a view or link refers to still exists, cleared once from any thread. */
using alive_flag = std::shared_ptr<std::atomic<bool> const>;

/** @brief A flag that is set, for its owner to clear with clear_alive(). */
[[nodiscard]] inline auto make_alive() -> std::shared_ptr<std::atomic<bool>>
{
  return std::make_shared<std::atomic<bool>>(true);
}

/**
 * @brief Reads a flag.
 * @param flag The flag.
 * @return Whether it is still set.
 */
[[nodiscard]] inline auto is_alive(alive_flag const& flag) noexcept -> bool
{
  return flag->load(std::memory_order_acquire);
}

/**
 * @brief Clears a flag.
 * @param flag The flag.
 */
inline void clear_alive(std::atomic<bool>& flag) noexcept
{
  flag.store(false, std::memory_order_release);
}

/**
 * @brief Refuses use of something whose machine is gone.
 * @param flag The flag.
 * @param key The config_error key.
 * @param what What was expected, as the config_error expectation.
 * @throws config_error when the flag is cleared.
 */
inline void require_alive(alive_flag const& flag, std::string_view key, std::string_view what)
{
  if(not is_alive(flag))
  {
    throw config_error(std::string{key}, 0, "machine", std::string{what});
  }
}
}
