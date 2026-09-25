#include <stdint.h>

extern uint32_t _estack;

volatile int32_t results[8];

__attribute__((noipa)) int32_t clamp(int32_t v, int32_t lo, int32_t hi)
{
  if(v < lo)
  {
    return lo;
  }
  if(v > hi)
  {
    return hi;
  }
  return v;
}

__attribute__((noipa)) uint32_t count_bits(uint32_t x)
{
  uint32_t n = 0;
  while(x != 0)
  {
    n += x & 1u;
    x >>= 1;
  }
  return n;
}

__attribute__((noipa)) int32_t classify(int32_t a, int32_t b)
{
  int32_t r = 0;
  if(a > 0 && b > 0)
  {
    r = 1;
  }
  else if(a < 0)
  {
    r = 2;
  }
  else
  {
    r = 3;
  }
  return r;
}

[[noreturn]] void lines_main(void)
{
  results[0] = clamp(5, 0, 10);
  results[1] = clamp(-1, 0, 10);
  results[2] = (int32_t)count_bits(0xbu);
  results[3] = classify(1, 1);
  results[4] = classify(-1, 7);
  __asm__ volatile(".global lines_done\n"
                   ".type lines_done, %%function\n"
                   "lines_done:\n"
                   "bkpt #0\n" ::
                       : "memory");
  for(;;)
  {
  }
}

void Reset_Handler(void) __attribute__((alias("lines_main"), noreturn));

__attribute__((section(".isr_vector"), used)) static const void* const vectors[] = {
    [0] = &_estack,
    [1] = (const void*)Reset_Handler,
};
