#pragma once

#include <stdint.h>

#define TARGET_OWN1      0x42u
#define TARGET_OWN2      0x60u
#define TARGET_ADDRESS   0xADD00000u
#define TARGET_STOP      0x5709u
#define TARGET_DATA      0xDA7A0000u
#define TARGET_NACK_NEXT 0xEEu
#define TARGET_TX_FIRST  0xB0u
#define TARGET_NACKED    0x4AC0u

extern volatile uint32_t i2c_target_active;

void target(void);
void i2c_target_event(void);
