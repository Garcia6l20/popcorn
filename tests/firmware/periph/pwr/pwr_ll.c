#include "periph_test.h"
#include "stm32u5xx_ll_bus.h"
#include "stm32u5xx_ll_pwr.h"

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

int main(void)
{
  LL_AHB3_GRP1_EnableClock(LL_AHB3_GRP1_PERIPH_PWR);
  LL_PWR_SetRegulVoltageScaling(LL_PWR_REGU_VOLTAGE_SCALE1);
  POLL(LL_PWR_IsActiveFlag_VOS());
  LL_PWR_EnableEPODBooster();
  POLL(LL_PWR_IsActiveFlag_BOOST());
  LL_PWR_EnableBkUpAccess();
  LOG(LL_PWR_GetRegulVoltageScaling());
  LOG(READ_BIT(PWR->SVMSR, PWR_SVMSR_ACTVOS) >> PWR_SVMSR_ACTVOS_Pos);
  DONE();
}
