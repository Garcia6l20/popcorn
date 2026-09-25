#include <stdint.h>

extern uint32_t _estack;
extern uint32_t types_touch(void);

static uint32_t shared_counter = 22;

[[noreturn]] void Reset_Handler(void)
{
  register uint32_t r0 __asm__("r0") = types_touch() + shared_counter;
  for(;;)
  {
    __asm__ volatile("bkpt #0" : : "r"(r0));
  }
}

__attribute__((section(".isr_vector"), used)) static const void* const vectors[] = {
    &_estack,
    (const void*)Reset_Handler,
};
