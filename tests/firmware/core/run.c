#include <stdint.h>

extern uint32_t _estack;
extern uint32_t _sidata[];
extern uint32_t _sdata[];
extern uint32_t _edata[];
extern uint32_t _sbss[];
extern uint32_t _ebss[];

volatile uint32_t order_log[8];
volatile uint32_t order_count;
volatile uint32_t ram_results[2];
__attribute__((aligned(4))) volatile uint16_t ram_fn[2];

static void log_order(uint32_t value)
{
  order_log[order_count++] = value;
}

void irq0_handler(void)
{
  log_order(0x10);
}

__attribute__((noipa)) void irq_order(void)
{
  __asm__ volatile("cpsid i" ::: "memory");
  log_order(1);
  __asm__ volatile("bkpt #2" ::: "memory");
  log_order(2);
  __asm__ volatile("cpsie i" ::: "memory");
  log_order(3);
  __asm__ volatile("bkpt #0" ::: "memory");
}

__attribute__((naked)) void load_unmapped(void)
{
  __asm__ volatile("ldr r1, =0x60000000\n"
                   "ldr r0, [r1]\n"
                   "bkpt #0\n"
                   ".ltorg\n");
}

__attribute__((naked)) void store_flash(void)
{
  __asm__ volatile("ldr r1, =0x08000000\n"
                   "str r0, [r1]\n"
                   "bkpt #0\n"
                   ".ltorg\n");
}

__attribute__((naked)) void undefined(void)
{
  __asm__ volatile("udf #0\n"
                   "bkpt #0\n");
}

__attribute__((naked)) void straight(void)
{
  __asm__ volatile(".global straight_mid\n"
                   ".type straight_mid, %function\n"
                   "movs r0, #1\n"
                   "movs r1, #2\n"
                   "movs r2, #3\n"
                   "movs r3, #4\n"
                   "straight_mid:\n"
                   "adds r0, r0, r1\n"
                   "adds r0, r0, r2\n"
                   "adds r0, r0, r3\n"
                   "adds r0, r0, #1\n"
                   "bkpt #0\n");
}

__attribute__((naked)) void ram_calls(void)
{
  __asm__ volatile("mov r4, r0\n"
                   "blx r4\n"
                   "ldr r5, =ram_results\n"
                   "str r0, [r5]\n"
                   "bkpt #1\n"
                   "blx r4\n"
                   "str r0, [r5, #4]\n"
                   "bkpt #0\n"
                   ".ltorg\n");
}

__attribute__((naked)) void guest_patch(void)
{
  __asm__ volatile("mov r4, r0\n"
                   "blx r4\n"
                   "ldr r5, =ram_results\n"
                   "str r0, [r5]\n"
                   "strh r2, [r1]\n"
                   "blx r4\n"
                   "str r0, [r5, #4]\n"
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

__attribute__((section(".isr_vector"), used)) static const void* const vectors[24] = {
    [0]  = &_estack,
    [1]  = (const void*)Reset_Handler,
    [16] = (const void*)irq0_handler,
    [17] = (const void*)irq_order,
    [18] = (const void*)load_unmapped,
    [19] = (const void*)store_flash,
    [20] = (const void*)undefined,
    [21] = (const void*)straight,
    [22] = (const void*)ram_calls,
    [23] = (const void*)guest_patch,
};
