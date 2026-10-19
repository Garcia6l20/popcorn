#include <stdint.h>

#include "lto.h"

extern uint32_t _estack;

static int32_t lto_main_count = 7;

static uint16_t lto_shared = 5;

[[noreturn]] void Reset_Handler(void)
{
  static uint32_t lto_ticks;
  for(;;)
  {
    uint32_t const lto_unused = lto_ticks * 3U;
    (void)lto_unused;
    lto_ticks  = lto_ticks + 1U;
    lto_shared = (uint16_t)(lto_shared + 1);
    lto_main_count += (int32_t)lto_bump((uint32_t)lto_main_count + lto_shared);
    __asm__ volatile("bkpt #0" : : : "memory");
  }
}

__attribute__((section(".isr_vector"), used)) static const void* const vectors[] = {
    &_estack,
    (const void*)Reset_Handler,
};
