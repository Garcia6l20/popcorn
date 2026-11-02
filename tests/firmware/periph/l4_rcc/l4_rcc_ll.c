#include "periph_test.h"
#include "stm32l4xx.h"
#include "stm32l4xx_ll_bus.h"
#include "stm32l4xx_ll_rcc.h"
#include "stm32l4xx_ll_system.h"

#define POLL(condition) \
  do                    \
  {                     \
    uint32_t polls = 0; \
    while(!(condition)) \
    {                   \
      ++polls;          \
    }                   \
    LOG(polls);         \
  } while(0)

void idle(void);

void idle(void)
{
  for(;;)
  {
    __WFI();
  }
}

static void log_clocks(void)
{
  SystemCoreClockUpdate();
  LOG(SystemCoreClock);
}

int main(void)
{
  LOG(RCC->CSR & 0xFF000000u);
  LL_RCC_ClearResetFlags();
  LOG(RCC->CSR & 0xFF000000u);
  log_clocks();

  LL_FLASH_SetLatency(LL_FLASH_LATENCY_2);
  LL_RCC_MSI_EnableRangeSelection();
  LL_RCC_MSI_SetRange(LL_RCC_MSIRANGE_11);
  log_clocks();

  LL_RCC_HSI_Enable();
  POLL(LL_RCC_HSI_IsReady());
  LL_RCC_SetSysClkSource(LL_RCC_SYS_CLKSOURCE_HSI);
  POLL(LL_RCC_GetSysClkSource() == LL_RCC_SYS_CLKSOURCE_STATUS_HSI);
  log_clocks();

  LL_RCC_HSE_Enable();
  POLL(LL_RCC_HSE_IsReady());
  LL_RCC_SetSysClkSource(LL_RCC_SYS_CLKSOURCE_HSE);
  POLL(LL_RCC_GetSysClkSource() == LL_RCC_SYS_CLKSOURCE_STATUS_HSE);
  LL_RCC_SetAHBPrescaler(LL_RCC_SYSCLK_DIV_2);
  LL_RCC_SetAPB1Prescaler(LL_RCC_APB1_DIV_4);
  LL_RCC_SetAPB2Prescaler(LL_RCC_APB2_DIV_1);
  log_clocks();
  LL_RCC_ClocksTypeDef clocks;
  LL_RCC_GetSystemClocksFreq(&clocks);
  LOG(clocks.SYSCLK_Frequency);
  LOG(clocks.HCLK_Frequency);
  LOG(clocks.PCLK1_Frequency);
  LOG(clocks.PCLK2_Frequency);

  LL_AHB2_GRP1_EnableClock(LL_AHB2_GRP1_PERIPH_GPIOB | LL_AHB2_GRP1_PERIPH_ADC);
  LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_SPI3);
  LL_APB2_GRP1_EnableClock(LL_APB2_GRP1_PERIPH_DFSDM1 | LL_APB2_GRP1_PERIPH_SDMMC1);
  LOG(RCC->AHB2ENR);
  LOG(RCC->APB1ENR1);
  LOG(RCC->APB2ENR);

  GPIOB->MODER = 0x5555u;
  LOG(GPIOB->MODER);
  LL_AHB2_GRP1_ForceReset(LL_AHB2_GRP1_PERIPH_GPIOB);
  LL_AHB2_GRP1_ReleaseReset(LL_AHB2_GRP1_PERIPH_GPIOB);
  LOG(GPIOB->MODER);
  DONE();
}
