#include "main.h"

#include "stm32u5xx.h"
#include "stm32u5xx_ll_adc.h"
#include "stm32u5xx_ll_bus.h"
#include "stm32u5xx_ll_pwr.h"
#include "stm32u5xx_ll_rcc.h"

volatile uint32_t vrefint_data;
volatile uint32_t vref_mv;
volatile uint32_t sensor_data;
volatile int32_t  temperature_c;
volatile uint32_t vrefint_data_12b;

__attribute__((noipa)) void adc_ready(void)
{}

__attribute__((noipa)) void measured(void)
{}

static void adc_power_up(void)
{
  LL_AHB3_GRP1_EnableClock(LL_AHB3_GRP1_PERIPH_PWR);
  LL_PWR_EnableVddA();
  LL_RCC_SetADCDACClockSource(LL_RCC_ADCDAC_CLKSOURCE_HCLK);
  LL_AHB2_GRP1_EnableClock(LL_AHB2_GRP1_PERIPH_ADC12);
  LL_ADC_SetCommonPathInternalCh(
      ADC12_COMMON, LL_ADC_PATH_INTERNAL_VREFINT | LL_ADC_PATH_INTERNAL_TEMPSENSOR);

  LL_ADC_DisableDeepPowerDown(ADC1);
  LL_ADC_EnableInternalRegulator(ADC1);
  while(LL_ADC_IsActiveFlag_LDORDY(ADC1) == 0)
  {
  }
  LL_ADC_StartCalibration(ADC1, LL_ADC_CALIB_OFFSET);
  while(LL_ADC_IsCalibrationOnGoing(ADC1) != 0)
  {
  }
}

static void adc_enable(void)
{
  LL_ADC_ClearFlag_ADRDY(ADC1);
  LL_ADC_Enable(ADC1);
  while(LL_ADC_IsActiveFlag_ADRDY(ADC1) == 0)
  {
  }
}

static void adc_disable(void)
{
  LL_ADC_Disable(ADC1);
  while(LL_ADC_IsEnabled(ADC1) != 0)
  {
  }
}

static uint32_t convert(uint32_t channel)
{
  LL_ADC_SetChannelPreselection(ADC1, channel);
  LL_ADC_SetChannelSamplingTime(ADC1, channel, LL_ADC_SAMPLINGTIME_814CYCLES);
  LL_ADC_REG_SetSequencerRanks(ADC1, LL_ADC_REG_RANK_1, channel);
  LL_ADC_REG_StartConversion(ADC1);
  while(LL_ADC_IsActiveFlag_EOC(ADC1) == 0)
  {
  }
  return LL_ADC_REG_ReadConversionData32(ADC1);
}

int main(void)
{
  adc_power_up();
  adc_enable();
  adc_ready();

  vrefint_data  = convert(LL_ADC_CHANNEL_VREFINT);
  vref_mv       = __LL_ADC_CALC_VREFANALOG_VOLTAGE(ADC1, vrefint_data, LL_ADC_RESOLUTION_14B);
  sensor_data   = convert(LL_ADC_CHANNEL_TEMPSENSOR);
  temperature_c = __LL_ADC_CALC_TEMPERATURE(ADC1, vref_mv, sensor_data, LL_ADC_RESOLUTION_14B);

  adc_disable();
  LL_ADC_SetResolution(ADC1, LL_ADC_RESOLUTION_12B);
  adc_enable();
  vrefint_data_12b = convert(LL_ADC_CHANNEL_VREFINT);

  measured();
  for(;;)
  {
    __WFI();
  }
}
