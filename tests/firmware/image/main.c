#include "shapes.h"

#include <stdint.h>

extern uint32_t _estack;
extern uint8_t  zero_buffer[64];

static const char message[] = "popcorn image firmware";

uint32_t global_counter = 0x1234;

__attribute__((noinline)) static uint32_t scale(uint32_t x)
{
  return x * 2;
}

[[noreturn]] void Reset_Handler(void)
{
  global_counter = scale(global_counter) + shapes_square(2) + shapes_perimeter(3);
  zero_buffer[0] = (uint8_t)message[0];
  register uint32_t r0 __asm__("r0") = global_counter;
  for(;;)
  {
    __asm__ volatile("bkpt #0" : : "r"(r0));
  }
}

__attribute__((section(".isr_vector"), used)) static const void* const vectors[] = {
    &_estack,
    (const void*)Reset_Handler,
};
