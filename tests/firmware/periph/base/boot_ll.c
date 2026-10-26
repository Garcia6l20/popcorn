#include "family.h"
#include "periph_test.h"

int main(void)
{
  LOG(SystemCoreClock);
  SystemCoreClockUpdate();
  LOG(SystemCoreClock);
  LL_RCC_ClocksTypeDef clocks;
  LL_RCC_GetSystemClocksFreq(&clocks);
  LOG(clocks.SYSCLK_Frequency);
  LOG(clocks.PCLK2_Frequency);
  LOG_RESET_CLOCK_SOURCE();
  DONE();
}
