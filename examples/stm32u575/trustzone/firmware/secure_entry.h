/**
 * @file
 * @brief The secure part's Non-secure callable entry functions. The secure part declares them
 * with `cmse_nonsecure_entry`, the Non-secure part calls their SG veneers.
 */
#ifndef SECURE_ENTRY_H
#define SECURE_ENTRY_H

#include <stdint.h>

#if defined(__ARM_FEATURE_CMSE) && __ARM_FEATURE_CMSE == 3
#define SECURE_ENTRY __attribute__((cmse_nonsecure_entry))
typedef void (*nonsecure_callback)(void) __attribute__((cmse_nonsecure_call));
#else
#define SECURE_ENTRY
typedef void (*nonsecure_callback)(void);
#endif

/**
 * @brief Add @p amount to the secure counter, kept in Secure SRAM4.
 * @return The new count.
 */
SECURE_ENTRY uint32_t secure_counter_add(uint32_t amount);

/**
 * @brief The number of illegal accesses `GTZC_IRQHandler` has counted.
 */
SECURE_ENTRY uint32_t secure_read_flag(void);

/**
 * @brief Load S0 to S31 with a pattern, call @p callback in Non-secure state, then compare.
 * @return The number of registers that kept their pattern, 32 when the FP context survived.
 */
SECURE_ENTRY uint32_t secure_fp_check(nonsecure_callback callback);

/**
 * @brief Start the Secure SysTick with @p reload processor clocks per tick.
 */
SECURE_ENTRY void secure_tick_start(uint32_t reload);

/**
 * @brief Execute an undefined instruction in Secure state.
 */
SECURE_ENTRY void secure_undefined(void);

#endif
