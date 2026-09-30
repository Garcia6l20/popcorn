#ifndef MAIN_H
#define MAIN_H

#include <stdint.h>

#define EEPROM_ADDRESS    (0x50u << 1)
#define EEPROM_PAGE       0x0100u
#define EEPROM_PAGE_BYTES 32u

/** @brief What the firmware wrote and read back, checked by the host. */
struct eeprom_result
{
  uint8_t  written[EEPROM_PAGE_BYTES];
  uint8_t  readback[EEPROM_PAGE_BYTES];
  uint32_t nacked_polls;
  uint32_t done;
};

extern struct eeprom_result eeprom_result;

/** @brief Called once eeprom_result is filled. */
void eeprom_done(void);

#endif
