#include <stdint.h>

extern uint32_t _estack;

static const char message[] = "popcorn smoke firmware";

static uint32_t fnv1a(const char* data, uint32_t size)
{
  uint32_t hash = 0x811c9dc5u;
  for(uint32_t i = 0; i < size; ++i)
  {
    hash ^= (uint8_t)data[i];
    hash *= 0x01000193u;
  }
  return hash;
}

[[noreturn]] void Reset_Handler(void)
{
  register uint32_t r0 __asm__("r0") = fnv1a(message, sizeof message - 1);
  for(;;)
  {
    __asm__ volatile("bkpt #0" : : "r"(r0));
  }
}

__attribute__((section(".isr_vector"), used)) static const void* const vectors[] = {
  &_estack,
  (const void*)Reset_Handler,
};
