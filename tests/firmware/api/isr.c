#include <stdint.h>

extern uint32_t _estack;
extern uint32_t _sidata[];
extern uint32_t _sdata[];
extern uint32_t _edata[];
extern uint32_t _sbss[];
extern uint32_t _ebss[];

#define NVIC_ISER0 (*(volatile uint32_t*)0xE000E100u)
#define NVIC_ISPR0 (*(volatile uint32_t*)0xE000E200u)
#define NVIC_IPR   ((volatile uint8_t*)0xE000E400u)

enum
{
  rounds     = 25,
  tail_irq   = 0,
  first_irq  = 1,
  second_irq = 2,
  low_irq    = 4,
  mid_irq    = 5,
  high_irq   = 6,
};

volatile uint32_t tail_entries;
volatile uint32_t exits;
volatile uint32_t first_entries;
volatile uint32_t first_leaves;
volatile uint32_t second_entries;
volatile uint32_t second_leaves;
volatile uint32_t low_entries;
volatile uint32_t low_leaves;
volatile uint32_t mid_entries;
volatile uint32_t mid_leaves;
volatile uint32_t high_entries;
volatile uint32_t high_leaves;
volatile uint32_t done;

__attribute__((noipa)) void irq_enter(volatile uint32_t* counter)
{
  *counter = *counter + 1u;
}

__attribute__((noipa)) uint32_t irq_exit(uint32_t irq)
{
  exits = exits + 1u;
  return exits * 10u + irq;
}

__attribute__((naked)) void tail_handler(void)
{
  __asm__ volatile("push {r3, lr}\n"
                   "ldr r0, =tail_entries\n"
                   "bl irq_enter\n"
                   "ldmia.w sp!, {r3, lr}\n"
                   "movs r0, #0\n"
                   "b.w irq_exit\n"
                   ".ltorg\n");
}

static void pend(uint32_t bits)
{
  NVIC_ISPR0 = bits;
  __asm__ volatile("dsb\nisb" ::: "memory");
}

void first_handler(void)
{
  irq_enter(&first_entries);
  first_leaves = first_leaves + 1u;
}

void second_handler(void)
{
  irq_enter(&second_entries);
  second_leaves = second_leaves + 1u;
}

void low_handler(void)
{
  irq_enter(&low_entries);
  pend(1u << mid_irq);
  low_leaves = low_leaves + 1u;
}

void mid_handler(void)
{
  irq_enter(&mid_entries);
  pend(1u << high_irq);
  mid_leaves = mid_leaves + 1u;
}

void high_handler(void)
{
  irq_enter(&high_entries);
  high_leaves = high_leaves + 1u;
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
  for(uint32_t irq = 0; irq <= high_irq; ++irq)
  {
    NVIC_IPR[irq] = 0xC0u;
  }
  NVIC_IPR[mid_irq]  = 0x80u;
  NVIC_IPR[high_irq] = 0x40u;
  NVIC_ISER0         = (1u << tail_irq)
                     | (1u << first_irq)
                     | (1u << second_irq)
                     | (1u << low_irq)
                     | (1u << mid_irq)
                     | (1u << high_irq);
  __asm__ volatile("cpsie i" ::: "memory");
  for(uint32_t round = 0; round < rounds; ++round)
  {
    pend(1u << tail_irq);
    pend((1u << first_irq) | (1u << second_irq));
    pend(1u << low_irq);
  }
  done = 1u;
  for(;;)
  {
    __asm__ volatile("wfi" ::: "memory");
  }
}

__attribute__((section(".isr_vector"), used)) const void* const vectors[23] = {
    [0]               = &_estack,
    [1]               = (const void*)Reset_Handler,
    [16 + tail_irq]   = (const void*)tail_handler,
    [16 + first_irq]  = (const void*)first_handler,
    [16 + second_irq] = (const void*)second_handler,
    [16 + low_irq]    = (const void*)low_handler,
    [16 + mid_irq]    = (const void*)mid_handler,
    [16 + high_irq]   = (const void*)high_handler,
};
