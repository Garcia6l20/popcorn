#include "periph_test.h"
#include "stm32u5xx_ll_rcc.h"

int main(void)
{
  LOG(SystemCoreClock);
  SystemCoreClockUpdate();
  LOG(SystemCoreClock);
  LL_RCC_ClocksTypeDef clocks;
  LL_RCC_GetSystemClocksFreq(&clocks);
  LOG(clocks.SYSCLK_Frequency);
  LOG(clocks.PCLK2_Frequency);
  LOG(LL_RCC_MSIS_GetRange());
  DONE();
}
