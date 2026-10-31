#include "periph_test.h"
#include "stm32l1xx.h"
#include "stm32l1xx_ll_adc.h"
#include "stm32l1xx_ll_bus.h"
#include "stm32l1xx_ll_rcc.h"

void idle(void);
void single(void);
void scan3(void);
void factory(void);

void idle(void)
{
  for(;;)
  {
    __WFI();
  }
}

static void enable(void)
{
  LL_RCC_HSI_Enable();
  while(!LL_RCC_HSI_IsReady())
  {
  }
  LL_APB2_GRP1_EnableClock(LL_APB2_GRP1_PERIPH_ADC1);
  LL_ADC_SetCommonClock(__LL_ADC_COMMON_INSTANCE(ADC1), LL_ADC_CLOCK_ASYNC_DIV1);
  LL_ADC_SetResolution(ADC1, LL_ADC_RESOLUTION_12B);
  LL_ADC_REG_SetFlagEndOfConversion(ADC1, LL_ADC_REG_FLAG_EOC_UNITARY_CONV);
  LL_ADC_Enable(ADC1);
  while(LL_ADC_IsActiveFlag_ADRDY(ADC1) == 0u)
  {
  }
}

static uint32_t convert(void)
{
  LL_ADC_REG_StartConversionSWStart(ADC1);
  while(LL_ADC_IsActiveFlag_EOCS(ADC1) == 0u)
  {
  }
  return LL_ADC_REG_ReadConversionData12(ADC1);
}

void single(void)
{
  enable();
  LL_ADC_REG_SetSequencerRanks(ADC1, LL_ADC_REG_RANK_1, LL_ADC_CHANNEL_5);
  LL_ADC_SetChannelSamplingTime(ADC1, LL_ADC_CHANNEL_5, LL_ADC_SAMPLINGTIME_16CYCLES);
  LOG(convert());
  LOG(ADC1->SR & (ADC_SR_STRT | ADC_SR_EOC | ADC_SR_ADONS));
  DONE();
}

void scan3(void)
{
  enable();
  LL_ADC_SetSequencersScanMode(ADC1, LL_ADC_SEQ_SCAN_ENABLE);
  LL_ADC_REG_SetSequencerLength(ADC1, LL_ADC_REG_SEQ_SCAN_ENABLE_3RANKS);
  LL_ADC_REG_SetSequencerRanks(ADC1, LL_ADC_REG_RANK_1, LL_ADC_CHANNEL_1);
  LL_ADC_REG_SetSequencerRanks(ADC1, LL_ADC_REG_RANK_2, LL_ADC_CHANNEL_18);
  LL_ADC_REG_SetSequencerRanks(ADC1, LL_ADC_REG_RANK_3, LL_ADC_CHANNEL_5);
  LL_ADC_SetChannelSamplingTime(ADC1, LL_ADC_CHANNEL_1, LL_ADC_SAMPLINGTIME_384CYCLES);
  LL_ADC_SetChannelSamplingTime(ADC1, LL_ADC_CHANNEL_18, LL_ADC_SAMPLINGTIME_384CYCLES);
  LL_ADC_SetChannelSamplingTime(ADC1, LL_ADC_CHANNEL_5, LL_ADC_SAMPLINGTIME_384CYCLES);
  LL_ADC_REG_StartConversionSWStart(ADC1);
  for(uint32_t rank = 0; rank < 3u; ++rank)
  {
    while(LL_ADC_IsActiveFlag_EOCS(ADC1) == 0u)
    {
    }
    LOG(LL_ADC_REG_ReadConversionData12(ADC1));
  }
  LOG(LL_ADC_IsActiveFlag_OVR(ADC1));
  DONE();
}

void factory(void)
{
  enable();
  LL_ADC_SetCommonPathInternalCh(
      __LL_ADC_COMMON_INSTANCE(ADC1),
      LL_ADC_PATH_INTERNAL_VREFINT | LL_ADC_PATH_INTERNAL_TEMPSENSOR);
  LL_ADC_SetChannelSamplingTime(ADC1, LL_ADC_CHANNEL_VREFINT, LL_ADC_SAMPLINGTIME_384CYCLES);
  LL_ADC_SetChannelSamplingTime(ADC1, LL_ADC_CHANNEL_TEMPSENSOR, LL_ADC_SAMPLINGTIME_384CYCLES);
  LL_ADC_REG_SetSequencerRanks(ADC1, LL_ADC_REG_RANK_1, LL_ADC_CHANNEL_VREFINT);
  uint32_t const vrefint = convert();
  uint32_t const vref_mv = __LL_ADC_CALC_VREFANALOG_VOLTAGE(vrefint, LL_ADC_RESOLUTION_12B);
  LL_ADC_REG_SetSequencerRanks(ADC1, LL_ADC_REG_RANK_1, LL_ADC_CHANNEL_TEMPSENSOR);
  uint32_t const sensor = convert();
  LOG(vrefint);
  LOG(vref_mv);
  LOG(sensor);
  LOG((uint32_t)__LL_ADC_CALC_TEMPERATURE(vref_mv, sensor, LL_ADC_RESOLUTION_12B));
  DONE();
}

int main(void)
{
  idle();
}
