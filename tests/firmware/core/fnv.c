#include <stdint.h>

extern uint32_t _estack;

enum
{
  hashed_bytes = 256u * 1024u,
  copied_bytes = 64u * 1024u,
};

volatile uint32_t fnv_result;
uint32_t          fnv_copy[copied_bytes / 4];

__attribute__((noipa)) uint32_t fnv1a(uint8_t const* bytes, uint32_t size)
{
  uint32_t hash = 2166136261u;
  for(uint32_t i = 0; i < size; ++i)
  {
    hash ^= bytes[i];
    hash *= 16777619u;
  }
  return hash;
}

__attribute__((noipa)) void copy_words(uint32_t* to, uint32_t const* from, uint32_t words)
{
  for(uint32_t i = 0; i < words; ++i)
  {
    to[i] = from[i];
  }
}

[[noreturn]] void fnv_entry(void)
{
  fnv_result = fnv1a((uint8_t const*)0x08000000u, hashed_bytes);
  copy_words(fnv_copy, (uint32_t const*)0x08000000u, copied_bytes / 4);
  fnv_result = fnv_result ^ fnv_copy[25];
  __asm__ volatile(".global fnv_done\n"
                   ".type fnv_done, %%function\n"
                   "fnv_done:\n"
                   "b fnv_done\n" ::
                       : "memory");
  for(;;)
  {
  }
}

void Reset_Handler(void) __attribute__((alias("fnv_entry"), noreturn));

__attribute__((section(".isr_vector"), used)) static const void* const vectors[] = {
    &_estack,
    (const void*)fnv_entry,
};
