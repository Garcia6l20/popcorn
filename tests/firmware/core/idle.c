#include <stdint.h>

extern uint32_t _estack;
extern uint32_t _sidata[];
extern uint32_t _sdata[];
extern uint32_t _edata[];
extern uint32_t _sbss[];
extern uint32_t _ebss[];

#define TIMER_CONTROL (*(volatile uint32_t*)0xE000E010u)
#define TIMER_PERIOD  (*(volatile uint32_t*)0xE000E014u)

enum
{
  instructions_per_millisecond = 4000,
};

volatile uint32_t ticks;

void tick_handler(void)
{
  ticks = ticks + 1;
}

static void start_timer(void)
{
  TIMER_PERIOD  = instructions_per_millisecond;
  TIMER_CONTROL = 1;
}

[[noreturn]] void idle_entry(void)
{
  start_timer();
  for(;;)
  {
    __asm__ volatile("wfi" ::: "memory");
  }
}

[[noreturn]] void idle_budget(void)
{
  start_timer();
  for(;;)
  {
    __asm__ volatile("movs r0, #50\n"
                     "1: subs r0, #1\n"
                     "bne 1b\n"
                     "wfi\n" ::
                         : "r0", "cc", "memory");
  }
}

[[noreturn]] void idle_forever(void)
{
  for(;;)
  {
    __asm__ volatile("wfi" ::: "memory");
  }
}

[[noreturn]] void idle_wide(void)
{
  for(;;)
  {
    __asm__ volatile("wfi.w" ::: "memory");
  }
}

[[noreturn]] void timer_spin(void)
{
  start_timer();
  for(;;)
  {
    __asm__ volatile("" ::: "memory");
  }
}

__attribute__((noipa)) void stop_timer(void)
{
  TIMER_CONTROL = 0;
}

__attribute__((noipa)) uint32_t spin(uint32_t loops)
{
  uint32_t sum = 0;
  for(uint32_t i = 0; i < loops; ++i)
  {
    __asm__ volatile("" : "+r"(sum));
    sum += i;
  }
  return sum;
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
  __asm__ volatile("bkpt #0" ::: "memory");
  for(;;)
  {
  }
}

__attribute__((section(".isr_vector"), used)) static const void* const vectors[24] = {
    [0]  = &_estack,
    [1]  = (const void*)Reset_Handler,
    [16] = (const void*)tick_handler,
    [17] = (const void*)idle_entry,
    [18] = (const void*)idle_budget,
    [19] = (const void*)idle_forever,
    [20] = (const void*)idle_wide,
    [21] = (const void*)timer_spin,
    [22] = (const void*)stop_timer,
    [23] = (const void*)spin,
};
