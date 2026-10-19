#include <stdint.h>

#include "lto.h"

extern uint32_t _estack;

static int32_t lto_main_count = 7;

[[noreturn]] void Reset_Handler(void)
{
  for(;;)
  {
    lto_main_count += (int32_t)lto_bump((uint32_t)lto_main_count);
    __asm__ volatile("bkpt #0" : : : "memory");
  }
}

__attribute__((section(".isr_vector"), used)) static const void* const vectors[] = {
    &_estack,
    (const void*)Reset_Handler,
};
