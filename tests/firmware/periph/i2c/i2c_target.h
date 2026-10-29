#pragma once

#include <stdint.h>

#define TARGET_OWN1       0x42u
#define TARGET_OWN2       0x60u
#define TARGET_ADDRESS    0xADD00000u
#define TARGET_STOP       0x5709u

extern volatile uint32_t i2c_target_active;

void target(void);
void i2c_target_event(void);
