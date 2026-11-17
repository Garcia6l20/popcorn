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

__attribute__((naked)) void wake_then_poll_hung(void)
{
  __asm__ volatile("cpsid i\n"
                   "ldr r1, =0xE000E010\n"
                   "ldr r0, =300\n"
                   "str r0, [r1, #4]\n"
                   "movs r0, #1\n"
                   "str r0, [r1]\n"
                   "ldr r1, =0x40020000\n"
                   "movs r2, #3\n"
                   "0: ldr r0, [r1, #4]\n"
                   "cmp r2, #0\n"
                   "beq 1f\n"
                   "subs r2, #1\n"
                   "wfi\n"
                   "1: tst r0, #1\n"
                   "bne 0b\n"
                   "bkpt #0\n"
                   ".ltorg\n");
}

__attribute__((naked)) void wake_then_wait_model(void)
{
  __asm__ volatile("cpsid i\n"
                   "ldr r1, =0xE000E010\n"
                   "ldr r0, =300\n"
                   "str r0, [r1, #4]\n"
                   "movs r0, #1\n"
                   "str r0, [r1]\n"
                   "ldr r1, =0x40010000\n"
                   "str r0, [r1, #0x5c]\n"
                   "movs r2, #3\n"
                   "0: ldr r0, [r1, #0x58]\n"
                   "cmp r2, #0\n"
                   "beq 1f\n"
                   "subs r2, #1\n"
                   "wfi\n"
                   "1: cmp r0, #0\n"
                   "beq 0b\n"
                   "bkpt #0\n"
                   ".ltorg\n");
}

__attribute__((naked)) void nested_poll(void)
{
  __asm__ volatile("ldr r1, =0x40020000\n"
                   "movs r3, #0\n"
                   "0: adds r3, #1\n"
                   "1: ldr r0, [r1, #4]\n"
                   "cmp r3, #5\n"
                   "blt 2f\n"
                   "tst r0, #1\n"
                   "bne 1b\n"
                   "2: b 0b\n"
                   ".ltorg\n");
}

__attribute__((naked)) void tick_walk(void)
{
  __asm__ volatile("ldr r1, =0x40010008\n"
                   "ldr r0, [r1]\n"
                   "movs r2, #3\n"
                   "1: subs r2, #1\n"
                   "bne 1b\n"
                   "bx lr\n"
                   ".ltorg\n");
}

__attribute__((naked)) void tick_hang(void)
{
  __asm__ volatile("ldr r1, =0x40020000\n"
                   "1: ldr r0, [r1, #4]\n"
                   "tst r0, #1\n"
                   "bne 1b\n"
                   "bx lr\n"
                   ".ltorg\n");
}

__attribute__((naked)) void poll_value(void)
{
  __asm__ volatile("ldr r1, =0x40010008\n"
                   "1: ldr r0, [r1]\n"
                   "cmp r0, #7\n"
                   "bne 1b\n"
                   "bkpt #0\n"
                   ".ltorg\n");
}

__attribute__((naked)) void poll_then_sleep(void)
{
  __asm__ volatile("cpsid i\n"
                   "ldr r1, =0x40010008\n"
                   "1: ldr r0, [r1]\n"
                   "cmp r0, #0\n"
                   "beq 1b\n"
                   "wfi\n"
                   "bkpt #0\n"
                   ".ltorg\n");
}

__attribute__((naked)) void widest_loop(void)
{
  __asm__ volatile("ldr r1, =0x40020000\n"
                   "1: ldr r0, [r1, #4]\n"
                   "b.n 2f\n"
                   ".space 250\n"
                   "2: b.n 1b\n"
                   ".ltorg\n");
}

__attribute__((naked)) void too_wide_loop(void)
{
  __asm__ volatile("ldr r1, =0x40020000\n"
                   "1: ldr r0, [r1, #4]\n"
                   "b.n 2f\n"
                   ".space 252\n"
                   "2: b.n 1b\n"
                   ".ltorg\n");
}

__attribute__((naked)) void delay_r4(void)
{
  __asm__ volatile("ldr r4, =1000000\n"
                   "1: subs r4, #1\n"
                   "bne 1b\n"
                   "bkpt #0\n"
                   ".ltorg\n");
}

__attribute__((naked)) void tick_wait_flag(void)
{
  __asm__ volatile("ldr r1, =0x40010008\n"
                   "1: ldr r0, [r1]\n"
                   "cmp r0, #0\n"
                   "beq 1b\n"
                   "bx lr\n"
                   ".ltorg\n");
}

__attribute__((naked)) void poll_discard(void)
{
  __asm__ volatile("ldr r1, =0x40010008\n"
                   "1: ldr r2, [r1]\n"
                   "movs r2, #0\n"
                   "b 1b\n"
                   ".ltorg\n");
}

__attribute__((naked)) void poll_exit_tail(void)
{
  __asm__ volatile("cpsid i\n"
                   "ldr r1, =0x40010008\n"
                   "1: ldr r0, [r1]\n"
                   "cmp r0, #0\n"
                   "beq 1b\n"
                   ".rept 40\n"
                   "nop\n"
                   ".endr\n"
                   "wfi\n"
                   "bkpt #0\n"
                   ".ltorg\n");
}

[[noreturn]] void Reset_Handler(void)
{
  __asm__ volatile("bkpt #0" ::: "memory");
  for(;;)
  {
  }
}

__attribute__((section(".isr_vector"), used)) static const void* const vectors[41] = {
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
    [30] = (const void*)wake_then_poll_hung,
    [31] = (const void*)wake_then_wait_model,
    [32] = (const void*)nested_poll,
    [33] = (const void*)poll_value,
    [34] = (const void*)poll_then_sleep,
    [35] = (const void*)widest_loop,
    [36] = (const void*)too_wide_loop,
    [37] = (const void*)delay_r4,
    [38] = (const void*)tick_wait_flag,
    [39] = (const void*)poll_discard,
    [40] = (const void*)poll_exit_tail,
};
