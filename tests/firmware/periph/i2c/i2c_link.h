#pragma once

#include <stdint.h>

extern volatile uint32_t link_target_active;

uint32_t link_wanted(void);
void     link_run(void);
void     link_target_event(void);
void     link_error_event(void);
