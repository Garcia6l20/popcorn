#include <stdint.h>

extern uint32_t _estack;

#define AIRCR    (*(volatile uint32_t*)0xE000ED0Cu)
#define UNMAPPED (*(volatile uint32_t*)0x60000000u)

volatile uint32_t alu_value;
volatile uint32_t alu_rounds;
volatile uint32_t spin_flag;

__attribute__((noipa)) void alu_mark(void)
{
  alu_rounds = alu_rounds + 1u;
}

[[noreturn]] void Reset_Handler(void)
{
  uint32_t value = 1;
  for(uint32_t round = 0;; ++round)
  {
    value     = value * 1664525u + 1013904223u;
    alu_value = value;
    if(round == 1000u)
    {
      alu_mark();
    }
  }
}

[[noreturn]] void idle_main(void)
{
  for(;;)
  {
    __asm__ volatile("wfi" ::: "memory");
  }
}

[[noreturn]] void fault_main(void)
{
  alu_value = UNMAPPED;
  for(;;)
  {
  }
}

[[noreturn]] void reset_main(void)
{
  AIRCR = (0x05FAu << 16) | (1u << 2);
  for(;;)
  {
  }
}

[[noreturn]] void spin_main(void)
{
  while(spin_flag == 0u)
  {
  }
  for(;;)
  {
  }
}

[[noreturn]] void bkpt_main(void)
{
  __asm__ volatile("bkpt #0" ::: "memory");
  for(;;)
  {
  }
}

#define VECTORS(name, entry) \
  __attribute__((aligned(128), used)) const void* const name[2] = {&_estack, (const void*)entry}

__attribute__((section(".isr_vector"), used)) const void* const vectors[2] = {
    &_estack,
    (const void*)Reset_Handler,
};
VECTORS(vt_idle, idle_main);
VECTORS(vt_fault, fault_main);
VECTORS(vt_reset, reset_main);
VECTORS(vt_spin, spin_main);
VECTORS(vt_bkpt, bkpt_main);
