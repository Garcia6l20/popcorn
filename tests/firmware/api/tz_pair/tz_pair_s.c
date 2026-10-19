#include <arm_cmse.h>
#include <stdint.h>

#define REG32(address) (*(volatile uint32_t*)(address))

#define SCB_NSACR       0xE000ED8Cu
#define SAU_CTRL        0xE000EDD0u
#define SAU_RNR         0xE000EDD8u
#define SAU_RBAR        0xE000EDDCu
#define SAU_RLAR        0xE000EDE0u
#define SCB_NS_VTOR     0xE002ED08u
#define MPCBB1_SECCFGR0 0x50032D00u
#define SRAM1_WORDS     12u
#define NS_VECTORS      0x08010000u
#define NS_FLASH_END    0x080FFFE0u
#define NS_SRAM         0x20000000u
#define NS_SRAM_END     0x2002FFE0u
#define REGION_ENABLE   1u
#define REGION_NSC      2u
#define NSACR_CP10_CP11 (3u << 10)

extern uint32_t _estack;
extern uint32_t __sgstubs_start;
extern uint32_t __sgstubs_end;

typedef void (*nonsecure_entry)(void) __attribute__((cmse_nonsecure_call));

void Reset_Handler(void);

__attribute__((section(".isr_vector"), used)) uintptr_t const vectors[16] = {
    (uintptr_t)&_estack,
    (uintptr_t)Reset_Handler,
};

__attribute__((cmse_nonsecure_entry, noipa)) int32_t tz_pair_add(int32_t a, int32_t b)
{
  return a + b;
}

static void sau_region(uint32_t number, uint32_t base, uint32_t limit, uint32_t flags)
{
  REG32(SAU_RNR)  = number;
  REG32(SAU_RBAR) = base;
  REG32(SAU_RLAR) = (limit & ~0x1Fu) | flags;
}

__attribute__((noipa)) int main(void)
{
  sau_region(0, NS_VECTORS, NS_FLASH_END, REGION_ENABLE);
  sau_region(1, NS_SRAM, NS_SRAM_END, REGION_ENABLE);
  sau_region(
      2, (uint32_t)&__sgstubs_start, (uint32_t)&__sgstubs_end - 1u, REGION_ENABLE | REGION_NSC);
  REG32(SAU_CTRL) = 1u;
  for(uint32_t word = 0; word < SRAM1_WORDS; ++word)
  {
    REG32(MPCBB1_SECCFGR0 + 4u * word) = 0;
  }
  REG32(SCB_NSACR) |= NSACR_CP10_CP11;
  REG32(SCB_NS_VTOR) = NS_VECTORS;
  __asm volatile("dsb\n isb" ::: "memory");
  uint32_t const stack = REG32(NS_VECTORS);
  __asm volatile("msr msp_ns, %0" ::"r"(stack));
  nonsecure_entry const entry = (nonsecure_entry)cmse_nsfptr_create(REG32(NS_VECTORS + 4u));
  entry();
  for(;;)
  {
  }
}

void Reset_Handler(void)
{
  main();
}
