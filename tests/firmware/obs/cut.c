#include <stdint.h>

extern uint32_t _estack;

volatile uint32_t sink;
volatile uint32_t faults;

__attribute__((noipa)) void note(uint32_t value)
{
  sink = value;
}

__attribute__((noipa)) uint32_t pick(uint32_t v)
{
  if(v > 3u)
  {
    note(v);
  }
  return v + 1u;
}

__attribute__((noipa)) uint32_t use_pick(uint32_t v)
{
  return pick(v) * 2u;
}

__attribute__((noipa)) void straight(uint32_t v)
{
  sink = v;
  __asm__ volatile(".global cut_mid\n"
                   "cut_mid:" ::
                       : "memory");
  if(sink > 9u)
  {
    note(0u);
  }
}

__attribute__((noipa)) uint32_t probe(uint32_t at)
{
  sink = 1u;
  if(*(volatile uint32_t const*)at == 5u)
  {
    note(5u);
  }
  return 0u;
}

void HardFault_Handler(void)
{
  faults = faults + 1u;
  for(;;)
  {
  }
}

[[noreturn]] void Reset_Handler(void)
{
  straight(12u);
  for(;;)
  {
  }
}

__attribute__((section(".isr_vector"), used)) static const void* const vectors[] = {
    [0] = &_estack,
    [1] = (const void*)Reset_Handler,
    [3] = (const void*)HardFault_Handler,
};
