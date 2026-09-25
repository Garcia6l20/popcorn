#include <stdint.h>

extern uint32_t _estack;
extern uint32_t _sidata[];
extern uint32_t _sdata[];
extern uint32_t _edata[];
extern uint32_t _sbss[];
extern uint32_t _ebss[];

#define TIMER_CONTROL (*(volatile uint32_t*)0xE000E010u)
#define TIMER_PERIOD  (*(volatile uint32_t*)0xE000E014u)
#define TESTDEV_DATA  (*(volatile uint32_t*)0x40010058u)
#define TESTDEV_CTRL  (*(volatile uint32_t*)0x4001005Cu)

volatile uint32_t                             ticks;
volatile uint32_t                             testdev_irqs;
volatile uint32_t                             state[64];
__attribute__((aligned(4))) volatile uint16_t ram_fn[2];
__attribute__((aligned(1024))) uint32_t       scratch[0x20000];

void tick_handler(void)
{
  ticks = ticks + 1u;
}

void testdev_handler(void)
{
  (void)TESTDEV_DATA;
  testdev_irqs = testdev_irqs + 1u;
}

__attribute__((naked)) void dirty(void)
{
  __asm__ volatile("1: str r1, [r0], #4\n"
                   "subs r1, #4\n"
                   "bne 1b\n"
                   "bx lr\n");
}

[[noreturn]] void work(void)
{
  uint32_t x = 1u;
  for(uint32_t i = 0;; ++i)
  {
    x              = x * 1664525u + 1013904223u;
    state[i & 63u] = state[i & 63u] ^ x;
    if((i & 1023u) == 0u)
    {
      TESTDEV_CTRL = 1u;
    }
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
  ram_fn[0]     = 0x2001u;
  ram_fn[1]     = 0x4770u;
  TIMER_PERIOD  = 997u;
  TIMER_CONTROL = 1u;
  __asm__ volatile("dsb\nisb\nbkpt #0" ::: "memory");
  work();
}

__attribute__((section(".isr_vector"), used)) static const void* const vectors[24] = {
    [0]  = &_estack,
    [1]  = (const void*)Reset_Handler,
    [16] = (const void*)tick_handler,
    [19] = (const void*)testdev_handler,
    [20] = (const void*)dirty,
    [21] = (const void*)work,
    [22] = (const void*)scratch,
};
