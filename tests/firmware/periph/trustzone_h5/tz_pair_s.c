#include "stm32h573xx.h"

#include <arm_cmse.h>
#include <stdint.h>

#define NS_VECTORS      0x08010000u
#define NS_FLASH_END    0x080FFFE0u
#define NS_SRAM_END     0x2002FFE0u
#define NS_PERIPH_END   0x4FFFFFE0u
#define SRAM1_WORDS     12u
#define NSACR_CP10_CP11 (SCB_NSACR_CP10_Msk | SCB_NSACR_CP11_Msk)

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
  SAU->RNR  = number;
  SAU->RBAR = base;
  SAU->RLAR = (limit & SAU_RLAR_LADDR_Msk) | flags;
}

__attribute__((noipa)) int main(void)
{
  sau_region(0, NS_VECTORS, NS_FLASH_END, SAU_RLAR_ENABLE_Msk);
  sau_region(1, SRAM1_BASE_NS, NS_SRAM_END, SAU_RLAR_ENABLE_Msk);
  sau_region(
      2, (uint32_t)&__sgstubs_start, (uint32_t)&__sgstubs_end - 1u,
      SAU_RLAR_ENABLE_Msk | SAU_RLAR_NSC_Msk);
  sau_region(3, PERIPH_BASE_NS, NS_PERIPH_END, SAU_RLAR_ENABLE_Msk);
  SAU->CTRL = SAU_CTRL_ENABLE_Msk;
  for(uint32_t word = 0; word < SRAM1_WORDS; ++word)
  {
    GTZC_MPCBB1_S->SECCFGR[word] = 0;
  }
  RCC_S->APB1LENR |= RCC_APB1LENR_TIM2EN | RCC_APB1LENR_TIM3EN;
  (void)RCC_S->APB1LENR;
  GTZC_TZSC1_S->SECCFGR1 = GTZC_CFGR1_TIM3_Msk;
  GTZC_TZIC1_S->IER1     = GTZC_CFGR1_TIM3_Msk;
  SCB->NSACR |= NSACR_CP10_CP11;
  SCB_NS->VTOR = NS_VECTORS;
  __DSB();
  __ISB();
  __TZ_set_MSP_NS(*(uint32_t const*)NS_VECTORS);
  nonsecure_entry const entry =
      (nonsecure_entry)cmse_nsfptr_create(*(uint32_t const*)(NS_VECTORS + 4u));
  entry();
  for(;;)
  {
  }
}

void Reset_Handler(void)
{
  main();
}
