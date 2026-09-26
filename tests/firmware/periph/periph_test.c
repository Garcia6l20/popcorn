#include "periph_test.h"

uint32_t log_buf[256];
uint32_t log_len;

/** @brief The test firmware is C and has no initialisers. The CMSIS startup calls it. */
void __libc_init_array(void);

void __libc_init_array(void)
{}
