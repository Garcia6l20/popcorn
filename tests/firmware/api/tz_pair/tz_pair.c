#include <stdint.h>

int32_t tz_pair_add(int32_t a, int32_t b);

extern uint32_t _estack;

volatile int32_t tz_pair_result;

void Reset_Handler(void);

__attribute__((section(".isr_vector"), used)) uintptr_t const vectors[16] = {
    (uintptr_t)&_estack,
    (uintptr_t)Reset_Handler,
};

__attribute__((noipa)) void tz_pair_done(void)
{
  for(;;)
  {
  }
}

__attribute__((noipa)) int main(void)
{
  tz_pair_result = tz_pair_add(40, 2);
  tz_pair_done();
  return 0;
}

void Reset_Handler(void)
{
  tz_pair_result = 0;
  main();
}
