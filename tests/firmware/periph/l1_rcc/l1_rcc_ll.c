#include "periph_test.h"
#include "stm32l1xx.h"
#include "stm32l1xx_ll_bus.h"
#include "stm32l1xx_ll_pwr.h"
#include "stm32l1xx_ll_rcc.h"
#include "stm32l1xx_ll_system.h"

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
  LOG(RCC->CSR & 0xFE000000u);
  LL_RCC_ClearResetFlags();
  LOG(RCC->CSR & 0xFE000000u);

  LL_RCC_MSI_SetRange(LL_RCC_MSIRANGE_6);
  SystemCoreClockUpdate();
  LOG(SystemCoreClock);

  LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_PWR);
  LL_PWR_SetRegulVoltageScaling(LL_PWR_REGU_VOLTAGE_SCALE1);
  POLL(LL_PWR_IsActiveFlag_VOS() == 0);

  LL_RCC_HSI_Enable();
  POLL(LL_RCC_HSI_IsReady());

  LL_FLASH_Enable64bitAccess();
  LL_FLASH_SetLatency(LL_FLASH_LATENCY_1);
  LL_RCC_PLL_ConfigDomain_SYS(LL_RCC_PLLSOURCE_HSI, LL_RCC_PLL_MUL_6, LL_RCC_PLL_DIV_3);
  LL_RCC_PLL_Enable();
  POLL(LL_RCC_PLL_IsReady());

  LL_RCC_SetAHBPrescaler(LL_RCC_SYSCLK_DIV_1);
  LL_RCC_SetAPB1Prescaler(LL_RCC_APB1_DIV_2);
  LL_RCC_SetAPB2Prescaler(LL_RCC_APB2_DIV_1);
  LL_RCC_SetSysClkSource(LL_RCC_SYS_CLKSOURCE_PLL);
  POLL(LL_RCC_GetSysClkSource() == LL_RCC_SYS_CLKSOURCE_STATUS_PLL);

  SystemCoreClockUpdate();
  LOG(SystemCoreClock);
  LL_RCC_ClocksTypeDef clocks;
  LL_RCC_GetSystemClocksFreq(&clocks);
  LOG(clocks.SYSCLK_Frequency);
  LOG(clocks.PCLK1_Frequency);
  LOG(clocks.PCLK2_Frequency);
  DONE();
}
