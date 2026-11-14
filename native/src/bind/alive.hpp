#pragma once

#include <popcorn/core/errors.hpp>
#include <popcorn/core/host.hpp>

#include <atomic>
#include <memory>
#include <string>
#include <string_view>

namespace popcorn::bind::detail
{
/** @brief A flag cleared once, from any thread, such as a machine's collected flag. */
using plain_flag = std::shared_ptr<std::atomic<bool> const>;

/**
 * @brief The flag of one machine: set while the machine exists, cleared once it is destroyed or
 *        closed.
 */
struct alive_state : std::atomic<bool>
{
  alive_state() noexcept : std::atomic<bool>{true}
  {}
  /** @brief Whether the flag was cleared by close_alive(), not by clear_alive(). */
  std::atomic<bool> closed{false};
};

/** @brief Whether what a view or link refers to still exists, cleared once from any thread. */
using alive_flag = std::shared_ptr<alive_state const>;

/** @brief A flag that is set, for its owner to clear with clear_alive() or close_alive(). */
[[nodiscard]] inline auto make_alive() -> std::shared_ptr<alive_state>
{
  return std::make_shared<alive_state>();
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
 * @brief Reads a plain flag.
 * @param flag The flag.
 * @return Whether it is still set.
 */
[[nodiscard]] inline auto is_alive(plain_flag const& flag) noexcept -> bool
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
 * @brief Clears a flag because its machine was closed, so require_alive() raises
 *        core::machine_closed from now on.
 * @param flag The flag.
 */
inline void close_alive(alive_state& flag) noexcept
{
  flag.closed.store(true, std::memory_order_release);
  flag.store(false, std::memory_order_release);
}

/**
 * @brief Whether a cleared flag was cleared by close_alive().
 * @param flag The machine's flag.
 */
[[nodiscard]] inline auto is_closed(alive_flag const& flag) noexcept -> bool
{
  return flag->closed.load(std::memory_order_acquire);
}

/**
 * @brief Refuses use of something whose machine is gone.
 * @param flag The flag.
 * @param key The config_error key.
 * @param what What was expected, as the config_error expectation.
 * @throws core::machine_closed when the flag was cleared by close_alive().
 * @throws config_error when the flag is cleared otherwise.
 */
inline void require_alive(alive_flag const& flag, std::string_view key, std::string_view what)
{
  if(not is_alive(flag))
  {
    if(is_closed(flag))
    {
      throw core::machine_closed("the machine was closed");
    }
    throw config_error(std::string{key}, 0, "machine", std::string{what});
  }
}
}
