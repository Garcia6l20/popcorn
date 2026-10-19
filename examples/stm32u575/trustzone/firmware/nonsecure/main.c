#include "secure_entry.h"
#include "stm32u5xx.h"

#include <stdint.h>

#define COUNTER_STEP       23u
#define TICK_RELOAD        1000u
#define SECURE_TICK_RELOAD 4000u

enum ns_case : uint32_t
{
  NS_CASE_COUNTER,
  NS_CASE_SECURE_BLOCK,
  NS_CASE_SECURE_ALIAS,
  NS_CASE_NO_VENEER,
  NS_CASE_FP,
  NS_CASE_TICKS,
  NS_CASE_UNDEFINED,
  NS_CASE_SECURE_UNDEFINED,
};

typedef uint32_t (*counter_function)(uint32_t);

extern uint32_t const __nonsecure_ram_end[];

volatile enum ns_case ns_case;
volatile uintptr_t    ns_target;
volatile uint32_t     ns_result;
volatile uint32_t     ns_flag;
volatile uint32_t     ns_ticks;

void              SysTick_Handler(void);
void              HardFault_Handler(void);
void              ns_fp_clobber(void);
[[noreturn]] void ns_hard_fault_spin(void);
[[noreturn]] void ns_ticking(void);
[[noreturn]] void ns_done(void);

void SysTick_Handler(void)
{
  ++ns_ticks;
}

__attribute__((noipa)) void ns_hard_fault_spin(void)
{
  for(;;)
  {
  }
}

void HardFault_Handler(void)
{
  ns_hard_fault_spin();
}

__attribute__((noipa)) void ns_ticking(void)
{
  for(;;)
  {
    __WFI();
  }
}

__attribute__((noipa)) void ns_done(void)
{
  for(;;)
  {
  }
}

static void run_case(enum ns_case selected)
{
  switch(selected)
  {
    case NS_CASE_COUNTER:
      ns_result = secure_counter_add(COUNTER_STEP);
      break;
    case NS_CASE_SECURE_BLOCK:
      ns_result = *(uint32_t const volatile*)__nonsecure_ram_end;
      while((ns_flag = secure_read_flag()) == 0u)
      {
      }
      break;
    case NS_CASE_SECURE_ALIAS:
      ns_result = *(uint32_t const volatile*)SRAM1_BASE_S;
      break;
    case NS_CASE_NO_VENEER:
      ns_result = ((counter_function)ns_target)(COUNTER_STEP);
      break;
    case NS_CASE_FP:
      ns_result = secure_fp_check(ns_fp_clobber);
      break;
    case NS_CASE_TICKS:
      SysTick_Config(TICK_RELOAD);
      secure_tick_start(SECURE_TICK_RELOAD);
      ns_ticking();
    case NS_CASE_UNDEFINED:
      __asm volatile("udf #0");
      break;
    case NS_CASE_SECURE_UNDEFINED:
      secure_undefined();
      break;
  }
}

int main(void)
{
  GPIOA->MODER = (GPIOA->MODER & ~GPIO_MODER_MODE5) | GPIO_MODER_MODE5_0;
  GPIOA->ODR ^= GPIO_ODR_OD5;
  run_case(ns_case);
  ns_done();
}
