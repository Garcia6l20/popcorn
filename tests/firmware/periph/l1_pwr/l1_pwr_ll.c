#include "periph_test.h"
#include "stm32l1xx.h"
#include "stm32l1xx_hal_pwr.h"
#include "stm32l1xx_ll_bus.h"
#include "stm32l1xx_ll_pwr.h"

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
  LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_PWR);
  LOG(LL_PWR_GetRegulVoltageScaling());
  LL_PWR_SetRegulVoltageScaling(LL_PWR_REGU_VOLTAGE_SCALE1);
  POLL(LL_PWR_IsActiveFlag_VOS() == 0);
  LOG(LL_PWR_GetRegulVoltageScaling());
  LL_PWR_SetRegulVoltageScaling(LL_PWR_REGU_VOLTAGE_SCALE3);
  POLL(LL_PWR_IsActiveFlag_VOS() == 0);
  LOG(LL_PWR_GetRegulVoltageScaling());
  *(__IO uint32_t*)CR_DBP_BB = (uint32_t)ENABLE;
  LOG(READ_BIT(PWR->CR, PWR_CR_DBP));
  DONE();
}
