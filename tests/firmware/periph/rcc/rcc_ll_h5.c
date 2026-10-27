#include "periph_test.h"
#include "stm32h5xx_ll_bus.h"
#include "stm32h5xx_ll_pwr.h"
#include "stm32h5xx_ll_rcc.h"
#include "stm32h5xx_ll_system.h"

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

int main(void)
{
  LL_PWR_SetRegulVoltageScaling(LL_PWR_REGU_VOLTAGE_SCALE0);
  POLL(LL_PWR_IsActiveFlag_VOS() == 1);
  LL_FLASH_SetLatency(LL_FLASH_LATENCY_5);
  POLL(LL_FLASH_GetLatency() == LL_FLASH_LATENCY_5);
  MODIFY_REG(FLASH->ACR, FLASH_ACR_WRHIGHFREQ, FLASH_ACR_WRHIGHFREQ_1);
  while(READ_BIT(FLASH->ACR, FLASH_ACR_WRHIGHFREQ) != FLASH_ACR_WRHIGHFREQ_1)
  {
  }

  LL_RCC_HSI_SetDivider(LL_RCC_HSI_DIV_2);
  POLL(LL_RCC_HSI_IsDividerReady() == 1);
  LL_RCC_HSI_Enable();
  POLL(LL_RCC_HSI_IsReady() == 1);

  LL_RCC_PLL1_ConfigDomain_SYS(LL_RCC_PLL1SOURCE_HSI, 2, 31, 2);
  LL_RCC_PLL1P_Enable();
  LL_RCC_PLL1_SetVCOInputRange(LL_RCC_PLLINPUTRANGE_8_16);
  LL_RCC_PLL1_SetVCOOutputRange(LL_RCC_PLLVCORANGE_WIDE);
  LL_RCC_PLL1FRACN_Enable();
  LL_RCC_PLL1_SetFRACN(2048);
  LL_RCC_PLL1_Enable();
  POLL(LL_RCC_PLL1_IsReady() == 1);

  LL_RCC_SetAHBPrescaler(LL_RCC_SYSCLK_DIV_2);
  LL_RCC_SetSysClkSource(LL_RCC_SYS_CLKSOURCE_PLL1);
  POLL(LL_RCC_GetSysClkSource() == LL_RCC_SYS_CLKSOURCE_STATUS_PLL1);
  LL_RCC_SetAHBPrescaler(LL_RCC_SYSCLK_DIV_1);
  LL_RCC_SetAPB1Prescaler(LL_RCC_APB1_DIV_1);
  LL_RCC_SetAPB2Prescaler(LL_RCC_APB2_DIV_1);
  LL_RCC_SetAPB3Prescaler(LL_RCC_APB3_DIV_1);

  LL_RCC_HSI48_Enable();
  POLL(LL_RCC_HSI48_IsReady() == 1);
  LL_RCC_SetUSBClockSource(LL_RCC_USB_CLKSOURCE_HSI48);
  LL_APB2_GRP1_EnableClock(LL_APB2_GRP1_PERIPH_USB);

  SystemCoreClockUpdate();
  LOG(SystemCoreClock);
  LL_RCC_ClocksTypeDef clocks;
  LL_RCC_GetSystemClocksFreq(&clocks);
  LOG(clocks.SYSCLK_Frequency);
  LOG(clocks.HCLK_Frequency);
  LOG(clocks.PCLK1_Frequency);
  DONE();
}
