#include "main.h"

#include "stm32h5xx.h"
#include "stm32h5xx_ll_bus.h"
#include "stm32h5xx_ll_pwr.h"
#include "stm32h5xx_ll_rcc.h"
#include "stm32h5xx_ll_system.h"

volatile uint32_t ticks;

__attribute__((noipa)) void hsi_ready(void)
{}

__attribute__((noipa)) void pll_ready(void)
{}

__attribute__((noipa)) void clock_configured(void)
{}

static void SystemClock_Config(void)
{
  LL_PWR_SetRegulVoltageScaling(LL_PWR_REGU_VOLTAGE_SCALE0);
  while(LL_PWR_IsActiveFlag_VOS() == 0)
  {
  }
  LL_FLASH_SetLatency(LL_FLASH_LATENCY_5);
  while(LL_FLASH_GetLatency() != LL_FLASH_LATENCY_5)
  {
  }

  LL_RCC_HSI_SetDivider(LL_RCC_HSI_DIV_2);
  while(LL_RCC_HSI_IsDividerReady() == 0)
  {
  }
  LL_RCC_HSI_Enable();
  while(LL_RCC_HSI_IsReady() == 0)
  {
  }
  hsi_ready();

  LL_RCC_PLL1_ConfigDomain_SYS(LL_RCC_PLL1SOURCE_HSI, 2, 31, 2);
  LL_RCC_PLL1P_Enable();
  LL_RCC_PLL1_SetVCOInputRange(LL_RCC_PLLINPUTRANGE_8_16);
  LL_RCC_PLL1_SetVCOOutputRange(LL_RCC_PLLVCORANGE_WIDE);
  LL_RCC_PLL1FRACN_Enable();
  LL_RCC_PLL1_SetFRACN(2048);
  LL_RCC_PLL1_Enable();
  while(LL_RCC_PLL1_IsReady() == 0)
  {
  }
  pll_ready();

  LL_RCC_SetAHBPrescaler(LL_RCC_SYSCLK_DIV_2);
  LL_RCC_SetSysClkSource(LL_RCC_SYS_CLKSOURCE_PLL1);
  while(LL_RCC_GetSysClkSource() != LL_RCC_SYS_CLKSOURCE_STATUS_PLL1)
  {
  }
  LL_RCC_SetAHBPrescaler(LL_RCC_SYSCLK_DIV_1);
  LL_RCC_SetAPB1Prescaler(LL_RCC_APB1_DIV_2);
  LL_RCC_SetAPB2Prescaler(LL_RCC_APB2_DIV_2);
  LL_RCC_SetAPB3Prescaler(LL_RCC_APB3_DIV_2);
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
