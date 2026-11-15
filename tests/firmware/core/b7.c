#include <stdint.h>

extern uint32_t _estack;
extern uint32_t _sidata[];
extern uint32_t _sdata[];
extern uint32_t _edata[];
extern uint32_t _sbss[];
extern uint32_t _ebss[];

#define SYST_CSR   (*(volatile uint32_t*)0xE000E010u)
#define SYST_RVR   (*(volatile uint32_t*)0xE000E014u)
#define SYST_CVR   (*(volatile uint32_t*)0xE000E018u)
#define DEMCR      (*(volatile uint32_t*)0xE000EDFCu)
#define DWT_CTRL   (*(volatile uint32_t*)0xE0001000u)
#define DWT_CYCCNT (*(volatile uint32_t*)0xE0001004u)

#define LOG_SLOTS 64u

volatile uint32_t uwTick;
volatile uint32_t passes;
volatile uint32_t log_count;
volatile uint32_t log_buf[LOG_SLOTS * 2];

void SysTick_Handler(void)
{
  uint32_t const slot = (log_count % LOG_SLOTS) * 2;
  log_buf[slot]       = SYST_CVR;
  log_buf[slot + 1]   = DWT_CYCCNT;
  log_count           = log_count + 1;
  uwTick              = uwTick + 1;
}

__attribute__((noinline)) uint32_t HAL_GetTick(void)
{
  return uwTick;
}

__attribute__((noinline)) void delay_inline(uint32_t ms)
{
  uint32_t const start = uwTick;
  while(uwTick - start < ms)
  {
  }
}

__attribute__((noinline)) void HAL_Delay(uint32_t ms)
{
  uint32_t const start = HAL_GetTick();
  uint32_t       wait  = ms;
  if(wait < 0xFFFFFFFFu)
  {
    wait += 1;
  }
  while((HAL_GetTick() - start) < wait)
  {
  }
}

__attribute__((noinline)) void wait_loop(void)
{
#if B7_MODE == 0
  delay_inline(10);
#elif B7_MODE == 1
  HAL_Delay(10);
#elif B7_MODE == 2
  __asm__ volatile("wait_here: b wait_here" ::: "memory");
#else
  __asm__ volatile("wfi" ::: "memory");
#endif
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
  DEMCR |= 1u << 24;
  DWT_CTRL |= 1u;
  SYST_RVR = 4000 - 1;
  SYST_CSR = 7;
  for(;;)
  {
    wait_loop();
    passes = passes + 1;
  }
}

__attribute__((section(".isr_vector"), used)) static const void* const vectors[16] = {
    [0]  = &_estack,
    [1]  = (const void*)Reset_Handler,
    [15] = (const void*)SysTick_Handler,
};
