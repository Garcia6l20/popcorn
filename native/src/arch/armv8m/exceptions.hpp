#pragma once

#include "state.hpp"

struct uc_struct;

namespace popcorn::arch::armv8m
{
/**
 * @brief Writes the special registers in the one order the engine accepts from any privilege:
 *        Handler mode, nPRIV cleared, privileged Thread, target SPSEL and FPCA, both stack
 *        pointers and the masks, target IPSR, target CONTROL with nPRIV last, then GE.
 * @param engine Engine to write.
 * @param target Values to apply.
 * @throws emulation_error when the engine refuses a write.
 */
void set_context(uc_struct* engine, special_registers const& target);

/**
 * @brief Reads the special registers from the engine, also in unprivileged Thread mode, where
 *        the engine reads the stack pointers and masks as zero: the read goes through Handler
 *        mode for its duration.
 * @throws emulation_error when the engine refuses a read or write.
 */
[[nodiscard]] auto read_special(uc_struct* engine) -> special_registers;
}
