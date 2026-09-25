#include <stdint.h>

extern uint32_t _estack;

__attribute__((naked)) void branches(void)
{
  __asm__ volatile(".global branch_top, branch_odd, branch_even, branch_join, branch_end\n"
                   ".type branch_top, %function\n"
                   ".type branch_odd, %function\n"
                   ".type branch_even, %function\n"
                   ".type branch_join, %function\n"
                   ".type branch_end, %function\n"
                   "movs r1, #0\n"
                   "branch_top:\n"
                   "tst r0, #1\n"
                   "beq branch_even\n"
                   "branch_odd:\n"
                   "adds r1, #1\n"
                   "b branch_join\n"
                   "branch_even:\n"
                   "adds r1, #2\n"
                   "b branch_join\n"
                   "branch_join:\n"
                   "subs r0, #1\n"
                   "bne branch_top\n"
                   "branch_end:\n"
                   "bkpt #0\n");
}

__attribute__((naked)) void outside(void)
{
  __asm__ volatile("movs r0, #4\n"
                   "b branches\n");
}

[[noreturn]] void Reset_Handler(void)
{
  __asm__ volatile("bkpt #0" ::: "memory");
  for(;;)
  {
  }
}

__attribute__((section(".isr_vector"), used)) static const void* const vectors[18] = {
    [0]  = &_estack,
    [1]  = (const void*)Reset_Handler,
    [16] = (const void*)branches,
    [17] = (const void*)outside,
};
