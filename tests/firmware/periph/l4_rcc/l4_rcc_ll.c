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

  LL_RCC_SetAHBPrescaler(LL_RCC_SYSCLK_DIV_1);
  LL_RCC_SetAPB1Prescaler(LL_RCC_APB1_DIV_1);
  LL_FLASH_SetLatency(LL_FLASH_LATENCY_4);
  LL_RCC_PLL_ConfigDomain_SYS(LL_RCC_PLLSOURCE_HSE, LL_RCC_PLLM_DIV_1, 20, LL_RCC_PLLR_DIV_2);
  LL_RCC_PLL_EnableDomain_SYS();
  LL_RCC_PLL_Enable();
  POLL(LL_RCC_PLL_IsReady());
  LL_RCC_SetSysClkSource(LL_RCC_SYS_CLKSOURCE_PLL);
  POLL(LL_RCC_GetSysClkSource() == LL_RCC_SYS_CLKSOURCE_STATUS_PLL);
  log_clocks();

  LL_RCC_SetSysClkSource(LL_RCC_SYS_CLKSOURCE_MSI);
  POLL(LL_RCC_GetSysClkSource() == LL_RCC_SYS_CLKSOURCE_STATUS_MSI);
  LL_RCC_PLL_Disable();
  POLL(!LL_RCC_PLL_IsReady());
  LL_RCC_PLL_ConfigDomain_SYS(LL_RCC_PLLSOURCE_MSI, LL_RCC_PLLM_DIV_6, 20, LL_RCC_PLLR_DIV_2);
  LL_RCC_PLL_Enable();
  POLL(LL_RCC_PLL_IsReady());
  LL_RCC_SetSysClkSource(LL_RCC_SYS_CLKSOURCE_PLL);
  POLL(LL_RCC_GetSysClkSource() == LL_RCC_SYS_CLKSOURCE_STATUS_PLL);
  log_clocks();

  LL_RCC_PLLSAI1_ConfigDomain_48M(
      LL_RCC_PLLSOURCE_MSI, LL_RCC_PLLM_DIV_6, 24, LL_RCC_PLLSAI1Q_DIV_4);
  LL_RCC_PLLSAI1_EnableDomain_48M();
  LL_RCC_PLLSAI1_Enable();
  POLL(LL_RCC_PLLSAI1_IsReady());
  LOG(RCC->PLLSAI1CFGR);
  DONE();
}
