#include <stdint.h>

extern uint32_t _estack;
extern uint32_t _sidata[];
extern uint32_t _sdata[];
extern uint32_t _edata[];
extern uint32_t _sbss[];
extern uint32_t _ebss[];

#define NVIC_ISER0 (*(volatile uint32_t*)0xE000E100u)

volatile uint32_t depth;
volatile uint32_t ticks;

__attribute__((noipa)) uint32_t level3(uint32_t value)
{
  depth = value;
  __asm__ volatile("udf #7" ::: "memory");
  return value + 3u;
}

__attribute__((noipa)) uint32_t level2(uint32_t value)
{
  uint32_t const below = level3(value + 1u);
  depth                = below;
  return below * 2u;
}

__attribute__((noipa)) uint32_t level1(uint32_t value)
{
  uint32_t const below = level2(value + 1u);
  depth                = below;
  return below * 3u;
}

__attribute__((noipa)) uint32_t in_handler(uint32_t value)
{
  ticks = value;
  __asm__ volatile("udf #9" ::: "memory");
  return value;
}

void IRQ0_Handler(void)
{
  ticks = in_handler(ticks + 1u) + 1u;
}

__attribute__((noipa)) void ready(void)
{
  __asm__ volatile("" ::: "memory");
}

__attribute__((noipa)) void worker(void)
{
  for(;;)
  {
    depth = depth + 1u;
  }
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
  NVIC_ISER0 = 1u;
  __asm__ volatile("cpsie i" ::: "memory");
  ready();
  worker();
  for(;;)
  {
  }
}

[[noreturn]] void nested_main(void)
{
  depth = level1(1u);
  for(;;)
  {
  }
}

__attribute__((aligned(128), used)) const void* const vt_nested[2] = {
    &_estack,
    (const void*)nested_main,
};

__attribute__((section(".isr_vector"), used)) const void* const vectors[17] = {
    [0]  = &_estack,
    [1]  = (const void*)Reset_Handler,
    [16] = (const void*)IRQ0_Handler,
};
