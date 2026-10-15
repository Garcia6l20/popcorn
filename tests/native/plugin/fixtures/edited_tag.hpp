#pragma once

#include <popcorn/plugin/abi.hpp>

/** @brief The layout of POPCORN_ABI_TAG_LITERAL with the compiler field @p GCC and the SDK
 * digest @p SDK in place of this unit's. */
#define EDITED_ABI_TAG(GCC, SDK)                                                                                                      \
  "popcorn " POPCORN_VERSION_STRING ";gcc " GCC                                                                                       \
  ";gxx-abi " POPCORN_ABI_STR(__GXX_ABI_VERSION) ";reflection " POPCORN_ABI_STR(__cpp_impl_reflection) ";contracts " POPCORN_ABI_STR( \
      __cpp_contracts) ";cxx11-abi " POPCORN_ABI_STR(_GLIBCXX_USE_CXX11_ABI) ";glibcxx-"                                              \
                                                                             "debug"                                                  \
                                                                             " " POPCORN_ABI_GLIBCXX_DEBUG                            \
                                                                             ";nanobind"                                              \
                                                                             " " POPCORN_NANOBIND_VERSION                             \
                                                                             ";nb-"                                                   \
                                                                             "domain"                                                 \
                                                                             " " POPCORN_NB_DOMAIN                                    \
                                                                             ";variant"                                               \
                                                                             " " POPCORN_VARIANT                                      \
                                                                             ";sdk " SDK                                              \
                                                                                 POPCORN_ABI_SANITIZER

/** @brief Defines the tag and name symbols of a hand-written fixture. */
#define FIXTURE_TAG_AND_NAME(TAG, NAME)                                                \
  extern "C" [[gnu::visibility("default"), gnu::used, gnu::section(".popcorn_abi")]]   \
  char const                                           popcorn_plugin_abi_tag[] = TAG; \
  extern "C" [[gnu::visibility("default")]] char const popcorn_plugin_name[]    = NAME
