#pragma once

/**
 * @file
 * @brief POPCORN_API marks an entity exported by a popcorn shared library. On ELF an export and
 *        an import are the same default visibility, so one macro serves the library and its
 *        consumers. POPCORN_HIDDEN marks a private nested type of an exported class, which would
 *        otherwise take the class's default visibility: its members stay local to the library,
 *        so link-time optimization may inline them.
 */

#define POPCORN_API    [[gnu::visibility("default")]]
#define POPCORN_HIDDEN [[gnu::visibility("hidden")]]
