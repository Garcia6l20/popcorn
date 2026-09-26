#include "periph_test.h"
#include "stm32u5xx_ll_bus.h"
#include "stm32u5xx_ll_rcc.h"

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
  LL_RCC_MSIS_Enable();
  POLL(LL_RCC_MSIS_IsReady());

  LL_RCC_PLL1_ConfigDomain_SYS(LL_RCC_PLL1SOURCE_MSIS, 1, 80, 2);
  LL_RCC_PLL1_SetVCOInputRange(LL_RCC_PLLINPUTRANGE_4_8);
  LL_RCC_PLL1_EnableDomain_SYS();
  LL_RCC_PLL1_Enable();
  POLL(LL_RCC_PLL1_IsReady());

  LL_RCC_SetAHBPrescaler(LL_RCC_SYSCLK_DIV_1);
  LL_RCC_SetAPB1Prescaler(LL_RCC_APB1_DIV_1);
  LL_RCC_SetAPB2Prescaler(LL_RCC_APB2_DIV_1);
  LL_RCC_SetAPB3Prescaler(LL_RCC_APB3_DIV_1);
  LL_RCC_SetSysClkSource(LL_RCC_SYS_CLKSOURCE_PLL1);
  POLL(LL_RCC_GetSysClkSource() == LL_RCC_SYS_CLKSOURCE_STATUS_PLL1);

  LL_RCC_HSE_Enable();
  POLL(LL_RCC_HSE_IsReady());
  LL_RCC_LSE_Enable();
  POLL(LL_RCC_LSE_IsReady());

  LL_RCC_PLL2_ConfigDomain_SAI(LL_RCC_PLL2SOURCE_HSE, 4, 48, 4);
  LL_RCC_PLL2_EnableDomain_SAI();
  LL_RCC_PLL2_Enable();
  POLL(LL_RCC_PLL2_IsReady());
  LL_RCC_PLL3_ConfigDomain_SAI(LL_RCC_PLL3SOURCE_HSE, 4, 48, 8);
  LL_RCC_PLL3_EnableDomain_SAI();
  LL_RCC_PLL3_Enable();
  POLL(LL_RCC_PLL3_IsReady());

  LL_RCC_SetUSARTClockSource(LL_RCC_USART1_CLKSOURCE_PCLK2);
  LL_APB2_GRP1_EnableClock(LL_APB2_GRP1_PERIPH_USART1);

  SystemCoreClockUpdate();
  LOG(SystemCoreClock);
  LL_RCC_ClocksTypeDef clocks;
  LL_RCC_GetSystemClocksFreq(&clocks);
  LOG(clocks.SYSCLK_Frequency);
  LOG(LL_RCC_GetUSARTClockFreq(LL_RCC_USART1_CLKSOURCE));
  DONE();
}
