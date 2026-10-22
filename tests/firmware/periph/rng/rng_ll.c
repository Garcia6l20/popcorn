#include "periph_test.h"
#include "stm32u5xx_ll_bus.h"
#include "stm32u5xx_ll_pwr.h"
#include "stm32u5xx_ll_rcc.h"
#include "stm32u5xx_ll_rng.h"

#define RNG_WORDS     16u
#define RNG_MAX_POLLS 2000u
#define RNG_NIST_CR   0x00F00D00u
#define RNG_NIST_HTCR 0x0000A2B0u
#define RNG_NIST_NSCR 0x00017CBBu

void idle(void);
void enable_read(void);
void nist_init(void);
void locked(void);
void no_kernel_clock(void);
void hsi16_source(void);
void pll_160(void);

void idle(void)
{
  for(;;)
  {
    __WFI();
  }
}

static void hsi48_on(void)
{
  LL_RCC_HSI48_Enable();
  while(!LL_RCC_HSI48_IsReady())
  {
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

static void read_words(void)
{
  LL_AHB2_GRP1_EnableClock(LL_AHB2_GRP1_PERIPH_RNG);
  LL_RNG_Enable(RNG);
  for(uint32_t word = 0; word < RNG_WORDS; ++word)
  {
    uint32_t const polls = poll_ready();
    LOG(RNG->SR);
    LOG(polls);
    LOG(LL_RNG_ReadRandData32(RNG));
  }
  LL_RNG_Disable(RNG);
}

void enable_read(void)
{
  hsi48_on();
  read_words();
  LL_RCC_HSI48_Disable();
  LOG(RNG->CR);
  DONE();
}

void nist_init(void)
{
  hsi48_on();
  LL_AHB2_GRP1_EnableClock(LL_AHB2_GRP1_PERIPH_RNG);
  RNG->CR        = RNG_NIST_CR | RNG_CR_CONDRST;
  RNG->HTCR      = RNG_NIST_HTCR;
  RNG->NSCR      = RNG_NIST_NSCR;
  RNG->CR        = RNG_NIST_CR;
  uint32_t polls = 0;
  while(LL_RNG_IsEnabledCondReset(RNG))
  {
    ++polls;
  }
  LOG(RNG->CR);
  LOG(RNG->HTCR);
  LOG(RNG->NSCR);
  LOG(polls);
  LL_RNG_Enable(RNG);
  LOG(poll_ready());
  LOG(RNG->SR);
  DONE();
}

void locked(void)
{
  hsi48_on();
  LL_AHB2_GRP1_EnableClock(LL_AHB2_GRP1_PERIPH_RNG);
  LL_RNG_ConfigLock(RNG);
  RNG->CR   = RNG_NIST_CR | RNG_CR_CONDRST;
  RNG->HTCR = RNG_NIST_HTCR;
  RNG->NSCR = RNG_NIST_NSCR;
  LOG(RNG->CR);
  LOG(RNG->HTCR);
  LOG(RNG->NSCR);
  DONE();
}

void no_kernel_clock(void)
{
  LL_AHB2_GRP1_EnableClock(LL_AHB2_GRP1_PERIPH_RNG);
  LL_RNG_Enable(RNG);
  LOG(poll_ready());
  LOG(RNG->SR);
  LOG(LL_RNG_ReadRandData32(RNG));
  DONE();
}

void hsi16_source(void)
{
  LL_RCC_HSI_Enable();
  while(!LL_RCC_HSI_IsReady())
  {
  }
  LL_RCC_SetRNGClockSource(LL_RCC_RNG_CLKSOURCE_HSI);
  read_words();
  LOG(LL_RCC_HSI48_IsReady());
  DONE();
}

void pll_160(void)
{
  LL_AHB3_GRP1_EnableClock(LL_AHB3_GRP1_PERIPH_PWR);
  LL_PWR_SetRegulVoltageScaling(LL_PWR_REGU_VOLTAGE_SCALE1);
  while(!LL_PWR_IsActiveFlag_VOS())
  {
  }
  LL_PWR_EnableEPODBooster();
  while(!LL_PWR_IsActiveFlag_BOOST())
  {
  }
  FLASH->ACR = (FLASH->ACR & ~FLASH_ACR_LATENCY) | 4u;
  LL_RCC_PLL1_ConfigDomain_SYS(LL_RCC_PLL1SOURCE_MSIS, 1, 80, 2);
  LL_RCC_PLL1_SetVCOInputRange(LL_RCC_PLLINPUTRANGE_4_8);
  LL_RCC_PLL1_EnableDomain_SYS();
  LL_RCC_PLL1_Enable();
  while(!LL_RCC_PLL1_IsReady())
  {
  }
  LL_RCC_SetSysClkSource(LL_RCC_SYS_CLKSOURCE_PLL1);
  while(LL_RCC_GetSysClkSource() != LL_RCC_SYS_CLKSOURCE_STATUS_PLL1)
  {
  }
  hsi48_on();
  LL_AHB2_GRP1_EnableClock(LL_AHB2_GRP1_PERIPH_RNG);
  DONE();
}

int main(void)
{
  idle();
}
