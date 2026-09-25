#include <stdint.h>

extern uint32_t _estack;
extern uint32_t _sidata[];
extern uint32_t _sdata[];
extern uint32_t _edata[];
extern uint32_t _sbss[];
extern uint32_t _ebss[];

volatile uint32_t                             results[8];
__attribute__((aligned(4))) volatile uint16_t ram_fn[2];

__attribute__((naked)) void cpu_accesses(void)
{
  __asm__ volatile("ldr r1, =0x40010000\n"
                   "ldr r2, =results\n"
                   "ldr r4, =0x50010000\n"
                   "ldr r0, =0xa5a5a5a5\n"
                   "str r0, [r1, #8]\n"
                   "ldr r3, [r1, #8]\n"
                   "str r3, [r2, #0]\n"
                   "movs r0, #0x3c\n"
                   "strb r0, [r1, #9]\n"
                   "ldr r3, [r1, #8]\n"
                   "str r3, [r2, #4]\n"
                   "movs r0, #0x77\n"
                   "strh r0, [r1, #0x14]\n"
                   "ldrh r3, [r1, #0x14]\n"
                   "str r3, [r2, #8]\n"
                   "ldrb r3, [r1, #0x3c]\n"
                   "str r3, [r2, #12]\n"
                   "ldr r3, [r1, #0x3c]\n"
                   "str r3, [r2, #16]\n"
                   "movs r0, #0x42\n"
                   "str r0, [r4, #8]\n"
                   "ldr r3, [r1, #8]\n"
                   "str r3, [r2, #20]\n"
                   "bkpt #0\n"
                   ".ltorg\n");
}

__attribute__((naked)) void model_access(void)
{
  __asm__ volatile(".global model_read, model_write\n"
                   ".type model_read, %function\n"
                   ".type model_write, %function\n"
                   "ldr r1, =0x40010000\n"
                   "movs r0, #1\n"
                   "nop\n"
                   "model_read:\n"
                   "ldr r3, [r1, #0x58]\n"
                   "nop\n"
                   "model_write:\n"
                   "str r0, [r1, #0x5c]\n"
                   "bkpt #0\n"
                   ".ltorg\n");
}

__attribute__((naked)) void fault_then_unmapped(void)
{
  __asm__ volatile("ldr r1, =0x40010000\n"
                   "ldr r2, =0x60000000\n"
                   "ldr r3, [r1, #0x58]\n"
                   "ldr r4, [r2]\n"
                   "bkpt #0\n"
                   ".ltorg\n");
}

__attribute__((naked)) void two_reads(void)
{
  __asm__ volatile("ldr r1, =0x40010000\n"
                   "ldr r3, [r1, #0x58]\n"
                   "ldr r3, [r1, #0x58]\n"
                   "bkpt #0\n"
                   ".ltorg\n");
}

__attribute__((naked)) void unmodelled_reads(void)
{
  __asm__ volatile("ldr r1, =0x40020000\n"
                   "ldr r3, [r1, #4]\n"
                   "ldr r3, [r1, #4]\n"
                   "ldr r3, [r1, #4]\n"
                   "bkpt #0\n"
                   ".ltorg\n");
}

__attribute__((naked)) void data_write(void)
{
  __asm__ volatile("ldr r1, =0x40010000\n"
                   "movs r0, #5\n"
                   "str r0, [r1, #0x58]\n"
                   "nop\n"
                   "nop\n"
                   "bkpt #0\n"
                   ".ltorg\n");
}

__attribute__((naked)) void dma_patch(void)
{
  __asm__ volatile("mov r5, r0\n"
                   "ldr r1, =0x40010000\n"
                   "ldr r2, =results\n"
                   "blx r5\n"
                   "str r0, [r2]\n"
                   "ldr r1, =0x40010000\n"
                   "movs r0, #1\n"
                   "str r0, [r1, #0x5c]\n"
                   "blx r5\n"
                   "ldr r2, =results\n"
                   "str r0, [r2, #4]\n"
                   "bkpt #0\n"
                   ".ltorg\n");
}

__attribute__((naked)) void rate_write(void)
{
  __asm__ volatile("ldr r1, =0x40010000\n"
                   "movs r0, #1\n"
                   "str r0, [r1, #0x5c]\n"
                   "1: b 1b\n"
                   ".ltorg\n");
}

__attribute__((naked)) void unaligned(void)
{
  __asm__ volatile("ldr r1, =0x40010000\n"
                   "ldr r0, =0x11223344\n"
                   "ldr.w r3, [r1, #0x59]\n"
                   "str.w r0, [r1, #0x59]\n"
                   "bkpt #0\n"
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
  ram_fn[0] = 0x2001u;
  ram_fn[1] = 0x4770u;
  __asm__ volatile("dsb\nisb\nbkpt #0" ::: "memory");
  for(;;)
  {
  }
}

__attribute__((section(".isr_vector"), used)) static const void* const vectors[26] = {
    [0]  = &_estack,
    [1]  = (const void*)Reset_Handler,
    [16] = (const void*)cpu_accesses,
    [17] = (const void*)model_access,
    [18] = (const void*)fault_then_unmapped,
    [19] = (const void*)two_reads,
    [20] = (const void*)unmodelled_reads,
    [21] = (const void*)data_write,
    [22] = (const void*)dma_patch,
    [23] = (const void*)rate_write,
    [24] = (const void*)unaligned,
};
