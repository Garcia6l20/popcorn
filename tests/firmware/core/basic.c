#include <stdint.h>

extern uint32_t _estack;
extern uint32_t _sidata[];
extern uint32_t _sdata[];
extern uint32_t _edata[];
extern uint32_t _sbss[];
extern uint32_t _ebss[];

volatile uint32_t initialised = 0xc0ffee42u;
volatile uint32_t pattern[16];
volatile uint32_t copied;
volatile uint32_t patched_result;

__attribute__((noipa)) uint32_t patched(void)
{
  return 1;
}

[[noreturn]] void Reset_Handler(void)
{
  for(uint32_t *from = _sidata, *to = _sdata; to < _edata;)
  {
    *to++ = *from++;
  }
  for(uint32_t* to = _sbss; to < _ebss;)
  {
    *to++ = 0;
  }
  for(uint32_t i = 0; i < 16; ++i)
  {
    pattern[i] = 0xa5a5'0000u | i;
  }
  copied         = initialised;
  patched_result = patched();
  __asm__ volatile("bkpt #0\n"
                   ".global patched_ret\n"
                   ".type patched_ret, %function\n"
                   ".thumb_func\n"
                   "patched_ret:\n"
                   "bkpt #1\n");
  for(;;)
  {
  }
}

__attribute__((section(".isr_vector"), used)) static const void* const vectors[] = {
    &_estack,
    (const void*)Reset_Handler,
};
