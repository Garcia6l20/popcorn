#include "main.h"

#include "stm32u5xx.h"
#include "stm32u5xx_ll_bus.h"
#include "stm32u5xx_ll_pwr.h"
#include "stm32u5xx_ll_rcc.h"
#include "stm32u5xx_ll_system.h"

volatile uint32_t ticks;

__attribute__((noipa)) void hse_ready(void)
{}

__attribute__((noipa)) void pll_ready(void)
{}

__attribute__((noipa)) void clock_configured(void)
{}

static void SystemClock_Config(void)
{
  LL_RCC_HSE_EnableBypass();
  LL_RCC_HSE_Enable();
  while(LL_RCC_HSE_IsReady() == 0)
  {
  }
  hse_ready();

  LL_RCC_PLL1_SetMainSource(LL_RCC_PLL1SOURCE_HSE);
  LL_RCC_SetPll1EPodPrescaler(LL_RCC_PLL1MBOOST_DIV_2);
  LL_AHB3_GRP1_EnableClock(LL_AHB3_GRP1_PERIPH_PWR);
  LL_PWR_SetRegulVoltageScaling(LL_PWR_REGU_VOLTAGE_SCALE1);
  while(LL_PWR_IsActiveFlag_VOS() == 0)
  {
  }
  LL_PWR_EnableEPODBooster();
  while(LL_PWR_IsActiveFlag_BOOST() == 0)
  {
  }
  LL_FLASH_SetLatency(LL_FLASH_LATENCY_4);
  while(LL_FLASH_GetLatency() != LL_FLASH_LATENCY_4)
  {
  }

  LL_RCC_PLL1_ConfigDomain_SYS(LL_RCC_PLL1SOURCE_HSE, 3, 30, 2);
  LL_RCC_PLL1_SetVCOInputRange(LL_RCC_PLLINPUTRANGE_8_16);
  LL_RCC_PLL1_EnableDomain_SYS();
  LL_RCC_PLL1_Enable();
  while(LL_RCC_PLL1_IsReady() == 0)
  {
  }
  pll_ready();

  LL_RCC_SetAHBPrescaler(LL_RCC_SYSCLK_DIV_1);
  LL_RCC_SetAPB1Prescaler(LL_RCC_APB1_DIV_1);
  LL_RCC_SetAPB2Prescaler(LL_RCC_APB2_DIV_1);
  LL_RCC_SetAPB3Prescaler(LL_RCC_APB3_DIV_1);
  LL_RCC_SetSysClkSource(LL_RCC_SYS_CLKSOURCE_PLL1);
  while(LL_RCC_GetSysClkSource() != LL_RCC_SYS_CLKSOURCE_STATUS_PLL1)
  {
  }
  SystemCoreClockUpdate();
  clock_configured();
}

void SysTick_Handler(void)
{
  ++ticks;
}

int main(void)
{
  SystemClock_Config();
  SysTick_Config(SystemCoreClock / 1000U);
  for(;;)
  {
    __WFI();
  }
}
