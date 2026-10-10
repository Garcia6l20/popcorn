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
volatile uint32_t results[4];

__attribute__((noipa)) uint32_t delay_ms(uint32_t ms)
{
  for(volatile uint32_t i = 0; i < ms * 10u; ++i)
  {
  }
  return ms;
}

__attribute__((noipa)) uint32_t use_delay(void)
{
  return delay_ms(1000) + 1;
}

__attribute__((noipa)) uint32_t seven(void)
{
  return 7;
}

__attribute__((noipa)) uint32_t call_seven(void)
{
  return seven() + 100;
}

__attribute__((noipa)) int32_t add(int32_t a, int32_t b)
{
  return a + b;
}

__attribute__((noipa, optimize("no-optimize-sibling-calls"))) uint32_t fib(uint32_t n)
{
  return n < 2 ? n : fib(n - 1) + fib(n - 2);
}

__attribute__((naked)) uint32_t six(void)
{
  __asm__ volatile("movs r0, #1\n"
                   "adds r0, #1\n"
                   "adds r0, #1\n"
                   "adds r0, #1\n"
                   "adds r0, #1\n"
                   "bx lr\n");
}

__attribute__((naked)) void loop_six(void)
{
  __asm__ volatile("1: bl six\n"
                   "subs r4, #1\n"
                   "bne 1b\n"
                   "bkpt #0\n");
}

__attribute__((naked)) void loop_nop(void)
{
  __asm__ volatile("1: nop.w\n"
                   "subs r4, #1\n"
                   "bne 1b\n"
                   "bkpt #0\n");
}

__attribute__((naked)) void bump(void)
{
  __asm__ volatile(".global bump_store\n"
                   ".type bump_store, %function\n"
                   "ldr r1, =counter\n"
                   "ldr r0, [r1]\n"
                   "adds r0, #1\n"
                   "bump_store:\n"
                   "str r0, [r1]\n"
                   "nop\n"
                   "ldr r2, [r1]\n"
                   "bkpt #0\n"
                   ".ltorg\n");
}

__attribute__((naked)) void poke_testdev(void)
{
  __asm__ volatile("ldr r1, =0x40010000\n"
                   "movs r0, #0x5a\n"
                   "str r0, [r1, #8]\n"
                   "bkpt #0\n"
                   ".ltorg\n");
}

__attribute__((noipa)) int32_t tail_g(int32_t x)
{
  return x * 3;
}

__attribute__((naked)) int32_t tail_f([[maybe_unused]] int32_t x)
{
  __asm__ volatile("adds r0, #1\n"
                   "b tail_g\n");
}

__attribute__((noipa, optimize("no-optimize-sibling-calls"))) int32_t call_tail(int32_t x)
{
  return tail_f(x) + 1000;
}

__attribute__((naked)) void halt(void)
{
  __asm__ volatile("bkpt #0\n");
}

const uint32_t flash_word = 0xC0DEF00Du;

__attribute__((naked)) void read_flash_alias(void)
{
  __asm__ volatile("ldr r1, =flash_word - 0x08000000\n"
                   "ldr r0, [r1]\n"
                   "bkpt #0\n"
                   ".ltorg\n");
}

__attribute__((naked)) void poke_sec_testdev(void)
{
  __asm__ volatile("ldr r1, =0x50010000\n"
                   "movs r0, #0xa5\n"
                   "str r0, [r1, #8]\n"
                   "bkpt #0\n"
                   ".ltorg\n");
}

__attribute__((noipa)) uint32_t resetting(void)
{
  if(BOOTS == 1u)
  {
    RESET_REQUEST = reset_key;
    for(;;)
    {
      __asm__ volatile("" ::: "memory");
    }
  }
  return 7;
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
  BOOTS      = BOOTS + 1u;
  results[0] = resetting();
  __asm__ volatile("bkpt #0" ::: "memory");
  for(;;)
  {
  }
}

volatile uint32_t isr_entries;
volatile uint32_t isr_exits;

__attribute__((noipa)) void isr_enter(void)
{
  isr_entries = isr_entries + 1u;
}

__attribute__((noipa)) uint32_t isr_exit(uint32_t source)
{
  isr_exits = isr_exits + 1u;
  return isr_exits * 10u + source;
}

__attribute__((naked)) void isr_tail(void)
{
  __asm__ volatile("push {r3, lr}\n"
                   "bl isr_enter\n"
                   "ldmia.w sp!, {r3, lr}\n"
                   "movs r0, #2\n"
                   "b.w isr_exit\n");
}

__attribute__((section(".isr_vector"), used)) static const void* const vectors[34] = {
    [0]  = &_estack,
    [1]  = (const void*)Reset_Handler,
    [16] = (const void*)use_delay,
    [17] = (const void*)call_seven,
    [18] = (const void*)add,
    [19] = (const void*)fib,
    [20] = (const void*)loop_six,
    [21] = (const void*)loop_nop,
    [22] = (const void*)bump,
    [23] = (const void*)poke_testdev,
    [24] = (const void*)delay_ms,
    [25] = (const void*)seven,
    [26] = (const void*)six,
    [27] = (const void*)resetting,
    [28] = (const void*)call_tail,
    [29] = (const void*)tail_f,
    [30] = (const void*)halt,
    [31] = (const void*)&flash_word,
    [32] = (const void*)read_flash_alias,
    [33] = (const void*)poke_sec_testdev,
};
