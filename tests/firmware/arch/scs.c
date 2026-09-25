#include "arch_test.h"

uint32_t results[64];
uint32_t result_count;

static void put(uint32_t value)
{
  results[result_count++] = value;
}

__attribute__((noipa)) void scs_run(void)
{
  put(REG32(0xE000ED00u));
  put(REG32(0xE000E004u));
  put(REG32(0xE000ED0Cu));
  REG32(0xE000ED0Cu) = 0x05FA0500u;
  put(REG32(0xE000ED0Cu));
  REG32(0xE000ED0Cu) = 0x12340300u;
  put(REG32(0xE000ED0Cu));
  REG32(0xE000E400u) = 0x12345678u;
  put(REG32(0xE000E400u));
  REG8(0xE000E408u) = 0xFFu;
  put(REG8(0xE000E408u));
  REG32(0xE000E100u) = 0xFFFFFFFFu;
  put(REG32(0xE000E100u));
  __asm__ volatile("cpsid i" ::: "memory");
  REG32(0xE000E200u) = 1u;
  put(REG32(0xE000E200u));
  REG32(0xE000E280u) = 1u;
  put(REG32(0xE000E200u));
  REG32(0xE000ED18u) = 0xFFFFFFFFu;
  put(REG32(0xE000ED18u));
  REG32(0xE000ED1Cu) = 0xFFFFFFFFu;
  put(REG32(0xE000ED1Cu));
  REG32(0xE000ED20u) = 0xFFFFFFFFu;
  put(REG32(0xE000ED20u));
  REG32(0xE000ED08u) = 0x2000007Fu;
  put(REG32(0xE000ED08u));
  put(REG32(0xE000ED14u));
  REG32(0xE000ED24u) = 0x00070000u;
  put(REG32(0xE000ED24u));
  REG32(0xE000ED88u) = 0x00F00000u;
  put(REG32(0xE000ED88u));
  put(REG32(0xE000EF34u));
  put(REG32(0xE000ED90u));
  REG32(0xE000ED98u) = 3u;
  REG32(0xE000ED9Cu) = 0x20001003u;
  REG32(0xE000ED98u) = 5u;
  REG32(0xE000ED9Cu) = 0x20002005u;
  REG32(0xE000ED98u) = 3u;
  put(REG32(0xE000ED9Cu));
  REG32(0xE000ED94u) = 5u;
  put(REG32(0xE000ED94u));
  REG32(0xE000EDFCu) = 0x01000000u;
  put(REG32(0xE000EDFCu));
  put(REG32(0xE0001000u));
  put(REG32(0xE0000000u));
  REG32(0xE000E014u) = 0xFFFFFFFFu;
  put(REG32(0xE000E014u));
  REG32(0xE000ED10u) = 0x16u;
  put(REG32(0xE000ED10u));
  REG32(0xE000E0F0u) = 0x1234u;
  put(REG32(0xE000E0F0u));
  __asm__ volatile("bkpt #1" ::: "memory");
}

__attribute__((noipa)) void reset_req(void)
{
  REG32(0xE000ED0Cu) = 0x05FA0004u;
  for(;;)
  {
    __asm__ volatile("" ::: "memory");
  }
}

__attribute__((noipa)) void reset_req_wrong_key(void)
{
  REG32(0xE000ED0Cu) = 0x12340004u;
  __asm__ volatile("bkpt #1" ::: "memory");
}

__attribute__((naked)) void unpriv_psp(void)
{
  __asm__ volatile("ldr r0, =0x20070000\n"
                   "msr psp, r0\n"
                   "movs r0, #2\n"
                   "msr control, r0\n"
                   "isb\n"
                   "movs r0, #0x40\n"
                   "msr basepri, r0\n"
                   "ldr r0, =0x000A0000\n"
                   "msr apsr_g, r0\n"
                   "movs r0, #3\n"
                   "msr control, r0\n"
                   "isb\n"
                   "bkpt #2\n"
                   ".ltorg\n");
}

__attribute__((naked)) void msplim_set(void)
{
  __asm__ volatile("ldr r0, =_estack - 0x100\n"
                   "msr msplim, r0\n"
                   "bkpt #3\n"
                   ".ltorg\n");
}

__attribute__((naked)) void deep_stack(void)
{
  __asm__ volatile("sub sp, #0x200\n"
                   "str r0, [sp]\n"
                   "add sp, #0x200\n"
                   "bkpt #4\n");
}
