#include "periph_test.h"
#include "stm32l4xx.h"
#include "stm32l4xx_ll_adc.h"
#include "stm32l4xx_ll_bus.h"
#include "stm32l4xx_ll_gpio.h"
#include "stm32l4xx_ll_rcc.h"

#define REGULATOR_SPIN   200u
#define CALIBRATION_SPIN 32u

void idle(void);
void aden_at_once(void);

void idle(void)
{
  for(;;)
  {
    __WFI();
  }
}

static void spin(uint32_t count)
{
  for(volatile uint32_t i = 0; i < count; ++i)
  {
  }
}

static void calibrate(void)
{
  LL_AHB2_GRP1_EnableClock(LL_AHB2_GRP1_PERIPH_GPIOA | LL_AHB2_GRP1_PERIPH_ADC);
  LL_RCC_SetADCClockSource(LL_RCC_ADC_CLKSOURCE_SYSCLK);
  LL_GPIO_SetPinMode(GPIOA, LL_GPIO_PIN_0, LL_GPIO_MODE_ANALOG);
  LL_GPIO_EnablePinAnalogControl(GPIOA, LL_GPIO_PIN_0);
  LL_ADC_SetCommonClock(__LL_ADC_COMMON_INSTANCE(ADC1), LL_ADC_CLOCK_ASYNC_DIV16);
  LL_ADC_SetCommonPathInternalCh(
      __LL_ADC_COMMON_INSTANCE(ADC1),
      LL_ADC_PATH_INTERNAL_VREFINT | LL_ADC_PATH_INTERNAL_TEMPSENSOR);
  LL_ADC_DisableDeepPowerDown(ADC1);
  LL_ADC_EnableInternalRegulator(ADC1);
  spin(REGULATOR_SPIN);
  LL_ADC_StartCalibration(ADC1, LL_ADC_SINGLE_ENDED);
  while(LL_ADC_IsCalibrationOnGoing(ADC1) != 0u)
  {
  }
}

static uint32_t convert(uint32_t channel)
{
  LL_ADC_REG_SetSequencerRanks(ADC1, LL_ADC_REG_RANK_1, channel);
  LL_ADC_REG_StartConversion(ADC1);
  while(LL_ADC_IsActiveFlag_EOC(ADC1) == 0u)
  {
  }
  return LL_ADC_REG_ReadConversionData12(ADC1);
}

void aden_at_once(void)
{
  calibrate();
  LL_ADC_Enable(ADC1);
  spin(REGULATOR_SPIN);
  LOG(LL_ADC_IsEnabled(ADC1));
  LOG(LL_ADC_IsActiveFlag_ADRDY(ADC1));
  DONE();
}

int main(void)
{
  calibrate();
  LOG(LL_ADC_GetCalibrationFactor(ADC1, LL_ADC_SINGLE_ENDED));
  spin(CALIBRATION_SPIN);
  LL_ADC_Enable(ADC1);
  while(LL_ADC_IsActiveFlag_ADRDY(ADC1) == 0u)
  {
  }
  LOG(LL_ADC_IsEnabled(ADC1));
  LOG(convert(LL_ADC_CHANNEL_5));
  uint32_t const vrefint = convert(LL_ADC_CHANNEL_VREFINT);
  LOG(vrefint);
  LOG(convert(LL_ADC_CHANNEL_TEMPSENSOR));
  LOG(__LL_ADC_CALC_VREFANALOG_VOLTAGE(vrefint, LL_ADC_RESOLUTION_12B));
  DONE();
}
