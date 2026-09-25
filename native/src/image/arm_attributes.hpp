#pragma once

#include <popcorn/arch/arch.hpp>

#include <cstddef>
#include <span>

namespace popcorn::image::detail
{
/**
 * @brief Reads the floating-point calling convention from a `.ARM.attributes` section.
 * @param attributes Section bytes: format-version byte `'A'` (0x41), then subsections, each
 *        holding vendor-scoped `Tag_File`/`Tag_Section`/`Tag_Symbol` scopes.
 * @return `hard_single` when the first `Tag_File` scope of the `aeabi` vendor subsection sets
 *         `Tag_ABI_HardFP_use` to 1, `hard_double` otherwise (absent, 0 "implied by
 *         Tag_FP_arch", or any other value): `Tag_FP_arch`'s numeric encoding does not
 *         distinguish single- from double-precision FPU variants, so it cannot stand in for
 *         `Tag_ABI_HardFP_use`. `hard_double` when the section is absent or says nothing.
 */
auto hard_float_abi(std::span<std::byte const> attributes) -> arch::float_abi;
}
