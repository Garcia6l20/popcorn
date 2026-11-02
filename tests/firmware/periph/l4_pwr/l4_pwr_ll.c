#include "periph_test.h"
#include "stm32l4xx.h"
#include "stm32l4xx_ll_bus.h"
#include "stm32l4xx_ll_cortex.h"
#include "stm32l4xx_ll_pwr.h"

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
void stop2(void);

void idle(void)
{
  for(;;)
  {
    __WFI();
  }
}

void stop2(void)
{
  LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_PWR);
  LL_PWR_SetPowerMode(LL_PWR_MODE_STOP2);
  LL_LPM_EnableDeepSleep();
  idle();
}

int main(void)
{
  LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_PWR);
  LOG(LL_PWR_GetRegulVoltageScaling());
  LL_PWR_SetRegulVoltageScaling(LL_PWR_REGU_VOLTAGE_SCALE2);
  POLL(LL_PWR_IsActiveFlag_VOS() == 0);
  LOG(LL_PWR_GetRegulVoltageScaling());
  LL_PWR_SetRegulVoltageScaling(LL_PWR_REGU_VOLTAGE_SCALE1);
  POLL(LL_PWR_IsActiveFlag_VOS() == 0);
  LOG(LL_PWR_GetRegulVoltageScaling());
  LL_PWR_EnableBkUpAccess();
  LOG(READ_BIT(PWR->CR1, PWR_CR1_DBP));
  RTC->BKP0R = 0xA5C3u;
  LOG(RTC->BKP0R);
  DONE();
}
