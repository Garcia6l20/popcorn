#include <stdint.h>

extern uint32_t _estack;

volatile uint32_t scratch;

__attribute__((naked)) void wait_ready(void)
{
  __asm__ volatile("ldr r1, =0x40020000\n"
                   "1: ldr r0, [r1, #4]\n"
                   "tst r0, #1\n"
                   "bne 1b\n"
                   "bkpt #0\n"
                   ".ltorg\n");
}

__attribute__((naked)) void delay(void)
{
  __asm__ volatile("ldr r0, =1000000\n"
                   "1: subs r0, #1\n"
                   "bne 1b\n"
                   "bkpt #0\n"
                   ".ltorg\n");
}

__attribute__((naked)) void wait_flag(void)
{
  __asm__ volatile("ldr r1, =0x40010008\n"
                   "1: ldr r0, [r1]\n"
                   "cmp r0, #0\n"
                   "beq 1b\n"
                   "bkpt #0\n"
                   ".ltorg\n");
}

__attribute__((naked)) void store_forever(void)
{
  __asm__ volatile("ldr r1, =scratch\n"
                   "movs r0, #5\n"
                   "1: str r0, [r1]\n"
                   "b 1b\n"
                   ".ltorg\n");
}

void tick_reads(void)
{
  (void)*(volatile uint32_t*)0x40010008u;
}

__attribute__((naked)) void wait_ready_ticking(void)
{
  __asm__ volatile("ldr r1, =0xE000E010\n"
                   "ldr r0, =500\n"
                   "str r0, [r1, #4]\n"
                   "movs r0, #1\n"
                   "str r0, [r1]\n"
                   "cpsie i\n"
                   "ldr r1, =0x40020000\n"
                   "1: ldr r0, [r1, #4]\n"
                   "tst r0, #1\n"
                   "bne 1b\n"
                   "bkpt #0\n"
                   ".ltorg\n");
}

__attribute__((naked)) void wait_model(void)
{
  __asm__ volatile("cpsid i\n"
                   "ldr r1, =0x40010000\n"
                   "movs r0, #1\n"
                   "str r0, [r1, #0x5c]\n"
                   "1: ldr r0, [r1, #0x58]\n"
                   "cmp r0, #0\n"
                   "beq 1b\n"
                   "bkpt #0\n"
                   ".ltorg\n");
}

__attribute__((naked)) void long_poll(void)
{
  __asm__ volatile("movs r2, #2\n"
                   "0: subs r2, #1\n"
                   "bne 0b\n"
                   "ldr r1, =0x40020000\n"
                   "1: ldr r0, [r1, #4]\n"
                   "b 2f\n"
                   ".space 300\n"
                   "2: b 1b\n"
                   ".ltorg\n");
}

__attribute__((naked)) void wait_helper(void)
{
  __asm__ volatile("cpsid i\n"
                   "ldr r1, =0x40010000\n"
                   "movs r0, #1\n"
                   "str r0, [r1, #0x5c]\n"
                   "1: bl 9f\n"
                   "cmp r0, #0\n"
                   "beq 1b\n"
                   "bkpt #0\n"
                   ".space 300\n"
                   "9: ldr r0, [r1, #0x58]\n"
                   "bx lr\n"
                   ".ltorg\n");
}

__attribute__((naked)) void wait_helper_hung(void)
{
  __asm__ volatile("ldr r1, =0x40020000\n"
                   "1: bl 9f\n"
                   "tst r0, #1\n"
                   "bne 1b\n"
                   "bkpt #0\n"
                   ".space 300\n"
                   "9: ldr r0, [r1, #4]\n"
                   "bx lr\n"
                   ".ltorg\n");
}

__attribute__((naked)) void wait_near_helper(void)
{
  __asm__ volatile("ldr r1, =0x40020000\n"
                   "1: bl 9f\n"
                   "tst r0, #1\n"
                   "bne 1b\n"
                   "bkpt #0\n"
                   "9: ldr r0, [r1, #4]\n"
                   "bx lr\n"
                   ".ltorg\n");
}

[[noreturn]] void Reset_Handler(void)
{
  __asm__ volatile("bkpt #0" ::: "memory");
  for(;;)
  {
  }
}

__attribute__((section(".isr_vector"), used)) static const void* const vectors[30] = {
    [0]  = &_estack,
    [1]  = (const void*)Reset_Handler,
    [16] = (const void*)tick_reads,
    [20] = (const void*)wait_ready,
    [21] = (const void*)delay,
    [22] = (const void*)wait_flag,
    [23] = (const void*)store_forever,
    [24] = (const void*)wait_ready_ticking,
    [25] = (const void*)wait_model,
    [26] = (const void*)long_poll,
    [27] = (const void*)wait_helper,
    [28] = (const void*)wait_helper_hung,
    [29] = (const void*)wait_near_helper,
};
