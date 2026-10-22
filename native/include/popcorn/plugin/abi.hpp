#pragma once

/**
 * @file
 * @brief The ABI tag of the translation unit that includes this header. It names the popcorn
 *        version and SDK digest the unit compiled against, and how the unit itself was compiled:
 *        compiler, C++ ABI version, reflection, contracts, libstdc++ string ABI, libstdc++ debug
 *        mode, nanobind, variant and sanitizer. A plugin embeds it, the loader compares it with
 *        popcorn::plugin::host_abi_tag() by string equality.
 */

#include <popcorn/plugin/build_info.hpp>

#include <cstddef>

#if not defined(__GXX_ABI_VERSION)
#error "a popcorn plugin needs a compiler defining __GXX_ABI_VERSION, GCC"
#endif
#if not defined(__cpp_impl_reflection)
#error "a popcorn plugin needs -freflection, __cpp_impl_reflection is not defined"
#endif
#if not defined(__cpp_contracts)
#error "a popcorn plugin needs contracts, __cpp_contracts is not defined"
#endif
#if not defined(_GLIBCXX_USE_CXX11_ABI)
#error "a popcorn plugin needs libstdc++, _GLIBCXX_USE_CXX11_ABI is not defined"
#endif

/** @brief Spells the expansion of a macro as a string literal. */
#define POPCORN_ABI_STR(x) POPCORN_ABI_STR_SPELL(x)
/** @brief Spells its argument as a string literal, unexpanded. */
#define POPCORN_ABI_STR_SPELL(x) #x

#if defined(_GLIBCXX_DEBUG)
/** @brief The libstdc++ debug mode field of the tag, it changes the layout of std containers. */
#define POPCORN_ABI_GLIBCXX_DEBUG "1"
#else
/** @brief The libstdc++ debug mode field of the tag, it changes the layout of std containers. */
#define POPCORN_ABI_GLIBCXX_DEBUG "0"
#endif

#if defined(NB_DOMAIN)
/** @brief The nanobind domain field of the tag, this unit's own `NB_DOMAIN`, empty without one. */
#define POPCORN_ABI_NB_DOMAIN POPCORN_ABI_STR(NB_DOMAIN)
#else
/** @brief The nanobind domain field of the tag, this unit's own `NB_DOMAIN`, empty without one. */
#define POPCORN_ABI_NB_DOMAIN ""
#endif

#if defined(__SANITIZE_ADDRESS__)
/** @brief The sanitizer field of the tag, empty without one. */
#define POPCORN_ABI_SANITIZER ";asan"
#elif defined(__SANITIZE_THREAD__)
/** @brief The sanitizer field of the tag, empty without one. */
#define POPCORN_ABI_SANITIZER ";tsan"
#else
/** @brief The sanitizer field of the tag, empty without one. */
#define POPCORN_ABI_SANITIZER ""
#endif

/** @brief The ABI tag as one string literal. */
#define POPCORN_ABI_TAG_LITERAL                                                                                                       \
  "popcorn " POPCORN_VERSION_STRING ";gcc " __VERSION__                                                                               \
  ";gxx-abi " POPCORN_ABI_STR(__GXX_ABI_VERSION) ";reflection " POPCORN_ABI_STR(__cpp_impl_reflection) ";contracts " POPCORN_ABI_STR( \
      __cpp_contracts) ";cxx11-abi " POPCORN_ABI_STR(_GLIBCXX_USE_CXX11_ABI) ";glibcxx-"                                              \
                                                                             "debug"                                                  \
                                                                             " " POPCORN_ABI_GLIBCXX_DEBUG                            \
                                                                             ";nanobind"                                              \
                                                                             " " POPCORN_NANOBIND_VERSION                             \
                                                                             ";nb-"                                                   \
                                                                             "domain"                                                 \
                                                                             " " POPCORN_ABI_NB_DOMAIN                                \
                                                                             ";variant"                                               \
                                                                             " " POPCORN_VARIANT                                      \
                                                                             ";sdk"                                                   \
                                                                             " " POPCORN_SDK_DIGEST                                   \
                                                                                 POPCORN_ABI_SANITIZER
