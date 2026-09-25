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
 * @return `soft` when the first `Tag_File` scope of the `aeabi` vendor subsection sets
 *         `Tag_ABI_VFP_args` to 0, the base AAPCS. `hard_double` otherwise, the section absent
 *         or silent included: AAPCS-VFP passes doubles in d registers whatever the FPU
 *         precision, so an FPU with single precision only, such as fpv5-sp-d16, still calls
 *         with the double convention. `Tag_ABI_VFP_args` 2, toolchain-specific, gives
 *         `hard_double` as a best guess, and 3, no floating-point arguments, gives `hard_double`
 *         harmlessly.
 */
auto hard_float_abi(std::span<std::byte const> attributes) -> arch::float_abi;
}
