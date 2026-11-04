#include "periph_test.h"
#include "stm32l4xx.h"
#include "stm32l4xx_ll_bus.h"
#include "stm32l4xx_ll_rcc.h"
#include "stm32l4xx_ll_rng.h"

#define RNG_WORDS     8u
#define RNG_MAX_POLLS 2000u
#define RNG_CR_CED_L4 (1u << 5)

volatile uint32_t irq_words;

void idle(void);
void interrupts(void);
void clock_error(void);
void clock_error_ced_off(void);
void RNG_IRQHandler(void);

void idle(void)
{
  for(;;)
  {
    __WFI();
  }
}

static uint32_t poll_ready(void)
{
  uint32_t polls = 0;
  while(!LL_RNG_IsActiveFlag_DRDY(RNG) && polls < RNG_MAX_POLLS)
  {
    ++polls;
  }
  return polls;
}

static void rng_clock(void)
{
  LL_RCC_SetRNGClockSource(LL_RCC_RNG_CLKSOURCE_MSI);
  LL_AHB2_GRP1_EnableClock(LL_AHB2_GRP1_PERIPH_RNG);
}

static void hclk_80mhz(void)
{
  FLASH->ACR = (FLASH->ACR & ~FLASH_ACR_LATENCY) | FLASH_ACR_LATENCY_4WS;
  LL_RCC_PLL_ConfigDomain_SYS(LL_RCC_PLLSOURCE_MSI, LL_RCC_PLLM_DIV_1, 40u, LL_RCC_PLLR_DIV_2);
  LL_RCC_PLL_EnableDomain_SYS();
  LL_RCC_PLL_Enable();
  while(!LL_RCC_PLL_IsReady())
  {
  }
  LL_RCC_SetSysClkSource(LL_RCC_SYS_CLKSOURCE_PLL);
  while(LL_RCC_GetSysClkSource() != LL_RCC_SYS_CLKSOURCE_STATUS_PLL)
  {
  }
}

int main(void)
{
  rng_clock();
  LL_RNG_Enable(RNG);
  for(uint32_t word = 0; word < RNG_WORDS; ++word)
  {
    uint32_t const polls = poll_ready();
    LOG(RNG->SR);
    LOG(polls);
    LOG(LL_RNG_ReadRandData32(RNG));
  }
  LL_RNG_Disable(RNG);
  LOG(RNG->CR);
  DONE();
}

void RNG_IRQHandler(void)
{
  if(LL_RNG_IsActiveFlag_DRDY(RNG))
  {
    LOG(LL_RNG_ReadRandData32(RNG));
    ++irq_words;
  }
}

void interrupts(void)
{
  rng_clock();
  NVIC_EnableIRQ(RNG_IRQn);
  LL_RNG_EnableIT(RNG);
  LL_RNG_Enable(RNG);
  while(irq_words < 4u)
  {
    __WFI();
  }
  LL_RNG_Disable(RNG);
  NVIC_DisableIRQ(RNG_IRQn);
  LOG(irq_words);
  DONE();
}

void clock_error(void)
{
  rng_clock();
  hclk_80mhz();
  LL_RNG_Enable(RNG);
  LOG(RNG->SR);
  LOG(poll_ready());
  LOG(RNG->SR);
  LOG(LL_RNG_ReadRandData32(RNG));
  LL_RNG_ClearFlag_CEIS(RNG);
  LOG(RNG->SR);
  LOG(poll_ready());
  LOG(LL_RNG_ReadRandData32(RNG));
  DONE();
}

void clock_error_ced_off(void)
{
  rng_clock();
  hclk_80mhz();
  RNG->CR = RNG_CR_CED_L4;
  RNG->CR = RNG_CR_CED_L4 | RNG_CR_RNGEN;
  LOG(RNG->SR);
  LOG(poll_ready());
  LOG(RNG->SR);
  LOG(LL_RNG_ReadRandData32(RNG));
  LOG(RNG->CR);
  DONE();
}
