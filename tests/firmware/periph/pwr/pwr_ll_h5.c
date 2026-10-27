#include "periph_test.h"
#include "stm32h5xx_ll_pwr.h"

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
  LOG(LL_PWR_GetRegulVoltageScaling());
  LL_PWR_SetRegulVoltageScaling(LL_PWR_REGU_VOLTAGE_SCALE0);
  POLL(LL_PWR_IsActiveFlag_VOS());
  LOG(LL_PWR_GetRegulVoltageScaling());
  LOG(READ_BIT(PWR->VOSSR, PWR_VOSSR_ACTVOS) >> PWR_VOSSR_ACTVOS_Pos);
  LOG(READ_BIT(PWR->VOSSR, PWR_VOSSR_ACTVOSRDY) != 0);
  LL_PWR_EnableAnalogBooster();
  LOG(LL_PWR_IsEnabledAnalogBooster());
  DONE();
}
