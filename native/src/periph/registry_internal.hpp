#pragma once

#include <popcorn/export.hpp>
#include <popcorn/periph/model.hpp>

#include <cstddef>
#include <string_view>

namespace popcorn::periph
{
/**
 * @brief Registers a factory the Python layer owns. Replaces an earlier Python registration of
 *        the same IP and glob and leaves the entries of any other origin below it, so removing
 *        it serves them again. At most one Python entry exists per IP and glob.
 * @param ip IP name.
 * @param ip_version_glob Glob over the IP version.
 * @param factory Factory called once per matching peripheral instance.
 */
POPCORN_API void register_python_model_factory(
    std::string_view ip,
    std::string_view ip_version_glob,
    model_factory    factory);

/**
 * @brief Removes the Python registration of an IP and glob.
 * @param ip IP name.
 * @param ip_version_glob Glob over the IP version.
 * @return Whether a Python registration existed.
 */
POPCORN_API auto
    unregister_python_model_factory(std::string_view ip, std::string_view ip_version_glob) -> bool;

/**
 * @brief Whether a registration of exactly this IP and glob exists, of any origin.
 * @param ip IP name.
 * @param ip_version_glob Glob, compared as text.
 */
[[nodiscard]] POPCORN_API auto
    has_registration(std::string_view ip, std::string_view ip_version_glob) -> bool;

/**
 * @brief The number of registrations of exactly this IP and glob, of any origin.
 * @param ip IP name.
 * @param ip_version_glob Glob, compared as text.
 */
[[nodiscard]] POPCORN_API auto
    registration_count(std::string_view ip, std::string_view ip_version_glob) -> std::size_t;
}
