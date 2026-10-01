#ifndef MAIN_H
#define MAIN_H

#include <stdint.h>

/** @brief Error flags of FLASH_NSSR after the page erase, 0 on success. */
extern volatile uint32_t erase_status;

/** @brief Error flags of FLASH_NSSR after the quad-word program, 0 on success. */
extern volatile uint32_t program_status;

/** @brief Error flags of FLASH_NSSR after a store into flash with PG clear. */
extern volatile uint32_t stray_status;

/** @brief Device electronic signature words, RM0456 chapter 76. */
struct device_signature
{
  /** @brief Flash size in KB, LL_GetFlashSize(). */
  uint32_t flash_kb;
  /** @brief PKG field of the package data register, LL_GetPackageType(). */
  uint32_t package;
  /** @brief The 96-bit unique device ID, word 0 first. */
  uint32_t uid[3];
};

/** @brief The signature as the firmware read it after flash_done(). */
extern volatile struct device_signature signature;

/** @brief Called once PLL1 drives SYSCLK at 160 MHz in voltage range 1. */
void clock_configured(void);

/** @brief Called once the data page is erased. */
void page_erased(void);

/** @brief Called once the record is programmed into the data page. */
void record_programmed(void);

/** @brief Called after the store without PG and the flash lock. */
void flash_done(void);

/** @brief Called once the device signature is read into signature. */
void signature_read(void);

/**
 * @brief Sets FLASH_ACR.LATENCY.
 * @param wait_states Wait states, 0 to 15.
 */
void set_latency(uint32_t wait_states);

#endif
