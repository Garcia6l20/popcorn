#pragma once

#include <string>
#include <string_view>

namespace popcorn::plugin::guard
{
/**
 * @brief Marks this thread as loading a plugin, from before the loader mutex until the load
 *        returns or throws. The destructor clears the declared-load watch as well.
 */
class loading
{
public:
  loading() noexcept;
  ~loading();
  loading(loading const&)                    = delete;
  auto operator=(loading const&) -> loading& = delete;
};

/** @brief Whether this thread is inside plugin::load, so a call now comes from a plugin. */
[[nodiscard]] auto in_load() noexcept -> bool;

/** @brief Watches global registrations of the declared plugin at @p path while its static
 * constructors run: they register nothing and mark the load violated. */
void watch_constructors(std::string_view path);

/** @brief Watches global registrations of the declared plugin at @p path while its init runs:
 * they register nothing, mark the load violated and throw config_error. */
void watch_init(std::string_view path);

/** @brief Marks the load in progress on this thread violated by @p call, unless it already is. */
void violate(std::string_view call) noexcept;

/** @brief The first call that violated the load in progress on this thread, empty for none. */
[[nodiscard]] auto violation() noexcept -> std::string_view;

/**
 * @brief Checks a global registration @p call, such as "periph::register_model".
 * @return Whether the caller must register nothing: a declared plugin's static constructors
 *         or init are running on this thread.
 * @throws config_error naming the plugin while a declared plugin's init runs.
 */
[[nodiscard]] auto refuses(std::string_view call) -> bool;
}
