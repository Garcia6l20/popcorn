#include <stdint.h>

extern uint32_t _estack;
extern uint32_t _sidata[];
extern uint32_t _sdata[];
extern uint32_t _edata[];
extern uint32_t _sbss[];
extern uint32_t _ebss[];

#define RESET_REQUEST (*(volatile uint32_t*)0xE000E00Cu)
#define BOOTS         (*(volatile uint32_t*)0x20080000u)

enum
{
  reset_key = 0x05FA0004u,
};

volatile uint32_t counter;

__attribute__((noipa)) int32_t add(int32_t a, int32_t b)
{
  return a + b;
}

__attribute__((noipa)) uint64_t mul64(uint64_t a, uint64_t b)
{
  return a * b;
}

[[noreturn]] __attribute__((noipa)) void spin_forever(void)
{
  for(;;)
  {
    __asm__ volatile("" ::: "memory");
  }
}

__attribute__((noipa)) int32_t fault_now(void)
{
  return *(volatile int32_t*)0x60000000u;
}

[[noreturn]] __attribute__((noipa)) void reset_now(void)
{
  RESET_REQUEST = reset_key;
  for(;;)
  {
    __asm__ volatile("" ::: "memory");
  }
}

__attribute__((naked)) void reset_then_fault(void)
{
  __asm__ volatile("ldr r1, =0xE000E00C\n"
                   "ldr r2, =0x05FA0004\n"
                   "str r2, [r1]\n"
                   "ldr r3, =0x60000000\n"
                   "ldr r0, [r3]\n"
                   "bkpt #0\n"
                   ".ltorg\n");
}

__attribute__((naked, noreturn)) void main_loop(void)
{
  __asm__ volatile(".global counter_loop\n"
                   ".type counter_loop, %function\n"
                   "ldr r1, =counter\n"
                   "counter_loop:\n"
                   "ldr r0, [r1]\n"
                   "adds r0, r0, #1\n"
                   "str r0, [r1]\n"
                   "b counter_loop\n"
                   ".ltorg\n");
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
  BOOTS = BOOTS + 1;
  main_loop();
}

__attribute__((section(".isr_vector"), used)) static const void* const vectors[23] = {
    [0]  = &_estack,
    [1]  = (const void*)Reset_Handler,
    [16] = (const void*)add,
    [17] = (const void*)mul64,
    [18] = (const void*)spin_forever,
    [19] = (const void*)fault_now,
    [20] = (const void*)reset_now,
    [21] = (const void*)reset_then_fault,
    [22] = (const void*)main_loop,
};
