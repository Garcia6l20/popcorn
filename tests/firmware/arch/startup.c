#include "arch_test.h"

extern uint32_t _estack;
extern uint32_t _sidata[];
extern uint32_t _sdata[];
extern uint32_t _edata[];
extern uint32_t _sbss[];
extern uint32_t _ebss[];

uint32_t log_buf[256];
uint32_t log_len;

[[noreturn]] void Reset_Handler(void)
{
  BOOTS = BOOTS + 1u;
  for(uint32_t *from = _sidata, *to = _sdata; to < _edata;)
  {
    *to++ = *from++;
  }
  for(uint32_t* to = _sbss; to < _ebss;)
  {
    *to++ = 0;
  }
  __asm__ volatile("bkpt #0" ::: "memory");
  for(;;)
  {
  }
}

__attribute__((naked)) void Default_Handler(void)
{
  __asm__ volatile("bkpt #0xfe\n"
                   "b Default_Handler\n");
}

void NMI_Handler(void) __attribute__((weak, alias("Default_Handler")));
void HardFault_Handler(void) __attribute__((weak, alias("Default_Handler")));
void MemManage_Handler(void) __attribute__((weak, alias("Default_Handler")));
void BusFault_Handler(void) __attribute__((weak, alias("Default_Handler")));
void UsageFault_Handler(void) __attribute__((weak, alias("Default_Handler")));
void SVC_Handler(void) __attribute__((weak, alias("Default_Handler")));
void DebugMon_Handler(void) __attribute__((weak, alias("Default_Handler")));
void PendSV_Handler(void) __attribute__((weak, alias("Default_Handler")));
void SysTick_Handler(void) __attribute__((weak, alias("Default_Handler")));
void IRQ0_Handler(void) __attribute__((weak, alias("Default_Handler")));
void IRQ1_Handler(void) __attribute__((weak, alias("Default_Handler")));
void IRQ2_Handler(void) __attribute__((weak, alias("Default_Handler")));
void IRQ3_Handler(void) __attribute__((weak, alias("Default_Handler")));
void IRQ4_Handler(void) __attribute__((weak, alias("Default_Handler")));
void IRQ5_Handler(void) __attribute__((weak, alias("Default_Handler")));
void IRQ6_Handler(void) __attribute__((weak, alias("Default_Handler")));
void IRQ7_Handler(void) __attribute__((weak, alias("Default_Handler")));

__attribute__((section(".isr_vector"), used)) const void* const vectors[24] = {
    &_estack,
    Reset_Handler,
    NMI_Handler,
    HardFault_Handler,
    MemManage_Handler,
    BusFault_Handler,
    UsageFault_Handler,
    0,
    0,
    0,
    0,
    SVC_Handler,
    DebugMon_Handler,
    0,
    PendSV_Handler,
    SysTick_Handler,
    IRQ0_Handler,
    IRQ1_Handler,
    IRQ2_Handler,
    IRQ3_Handler,
    IRQ4_Handler,
    IRQ5_Handler,
    IRQ6_Handler,
    IRQ7_Handler,
};
