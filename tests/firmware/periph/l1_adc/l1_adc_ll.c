#include "periph_test.h"
#include "stm32l1xx.h"
#include "stm32l1xx_ll_adc.h"
#include "stm32l1xx_ll_bus.h"
#include "stm32l1xx_ll_dma.h"
#include "stm32l1xx_ll_rcc.h"
#include "stm32l1xx_ll_tim.h"

void idle(void);
void single(void);
void scan3(void);
void triggered(void);
void injected(void);
void dma3(void);
void watchdog(void);
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

static void three_ranks(void)
{
  LL_ADC_SetSequencersScanMode(ADC1, LL_ADC_SEQ_SCAN_ENABLE);
  LL_ADC_REG_SetSequencerLength(ADC1, LL_ADC_REG_SEQ_SCAN_ENABLE_3RANKS);
  LL_ADC_REG_SetSequencerRanks(ADC1, LL_ADC_REG_RANK_1, LL_ADC_CHANNEL_1);
  LL_ADC_REG_SetSequencerRanks(ADC1, LL_ADC_REG_RANK_2, LL_ADC_CHANNEL_18);
  LL_ADC_REG_SetSequencerRanks(ADC1, LL_ADC_REG_RANK_3, LL_ADC_CHANNEL_5);
  LL_ADC_SetChannelSamplingTime(ADC1, LL_ADC_CHANNEL_1, LL_ADC_SAMPLINGTIME_384CYCLES);
  LL_ADC_SetChannelSamplingTime(ADC1, LL_ADC_CHANNEL_18, LL_ADC_SAMPLINGTIME_384CYCLES);
  LL_ADC_SetChannelSamplingTime(ADC1, LL_ADC_CHANNEL_5, LL_ADC_SAMPLINGTIME_384CYCLES);
}

static void log_ranks(uint32_t count)
{
  for(uint32_t rank = 0; rank < count; ++rank)
  {
    while(LL_ADC_IsActiveFlag_EOCS(ADC1) == 0u)
    {
    }
    LOG(LL_ADC_REG_ReadConversionData12(ADC1));
  }
}

void scan3(void)
{
  enable();
  three_ranks();
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

void triggered(void)
{
  enable();
  three_ranks();
  LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_TIM2);
  LL_TIM_SetAutoReload(TIM2, 1999u);
  LL_TIM_SetTriggerOutput(TIM2, LL_TIM_TRGO_UPDATE);
  LL_ADC_REG_SetTriggerSource(ADC1, LL_ADC_REG_TRIG_EXT_TIM2_TRGO);
  LL_ADC_REG_StartConversionExtTrig(ADC1, LL_ADC_REG_TRIG_EXT_RISING);
  LL_TIM_EnableCounter(TIM2);
  log_ranks(6u);
  LOG(LL_ADC_IsActiveFlag_OVR(ADC1));
  DONE();
}

void injected(void)
{
  enable();
  LL_ADC_REG_SetSequencerRanks(ADC1, LL_ADC_REG_RANK_1, LL_ADC_CHANNEL_1);
  LL_ADC_SetChannelSamplingTime(ADC1, LL_ADC_CHANNEL_1, LL_ADC_SAMPLINGTIME_384CYCLES);
  LL_ADC_SetChannelSamplingTime(ADC1, LL_ADC_CHANNEL_5, LL_ADC_SAMPLINGTIME_16CYCLES);
  LL_ADC_INJ_SetSequencerRanks(ADC1, LL_ADC_INJ_RANK_4, LL_ADC_CHANNEL_5);
  LL_ADC_INJ_SetOffset(ADC1, LL_ADC_INJ_RANK_1, 0x400u);
  LL_ADC_REG_StartConversionSWStart(ADC1);
  LL_ADC_INJ_StartConversionSWStart(ADC1);
  while(LL_ADC_IsActiveFlag_JEOS(ADC1) == 0u)
  {
  }
  LOG(LL_ADC_INJ_ReadConversionData12(ADC1, LL_ADC_INJ_RANK_1));
  LOG(LL_ADC_IsActiveFlag_EOCS(ADC1));
  while(LL_ADC_IsActiveFlag_EOCS(ADC1) == 0u)
  {
  }
  LOG(LL_ADC_REG_ReadConversionData12(ADC1));
  DONE();
}

static uint16_t samples[3];

void dma3(void)
{
  enable();
  three_ranks();
  LL_AHB1_GRP1_EnableClock(LL_AHB1_GRP1_PERIPH_DMA1);
  LL_DMA_ConfigTransfer(
      DMA1, LL_DMA_CHANNEL_1,
      LL_DMA_DIRECTION_PERIPH_TO_MEMORY
          | LL_DMA_MODE_NORMAL
          | LL_DMA_PERIPH_NOINCREMENT
          | LL_DMA_MEMORY_INCREMENT
          | LL_DMA_PDATAALIGN_HALFWORD
          | LL_DMA_MDATAALIGN_HALFWORD
          | LL_DMA_PRIORITY_HIGH);
  LL_DMA_ConfigAddresses(
      DMA1, LL_DMA_CHANNEL_1, LL_ADC_DMA_GetRegAddr(ADC1, LL_ADC_DMA_REG_REGULAR_DATA),
      (uint32_t)samples, LL_DMA_DIRECTION_PERIPH_TO_MEMORY);
  LL_DMA_SetDataLength(DMA1, LL_DMA_CHANNEL_1, 3u);
  LL_DMA_EnableChannel(DMA1, LL_DMA_CHANNEL_1);
  LL_ADC_REG_SetDMATransfer(ADC1, LL_ADC_REG_DMA_TRANSFER_LIMITED);
  LL_ADC_REG_StartConversionSWStart(ADC1);
  while(LL_DMA_IsActiveFlag_TC1(DMA1) == 0u)
  {
  }
  LOG(samples[0]);
  LOG(samples[1]);
  LOG(samples[2]);
  LOG(LL_ADC_IsActiveFlag_OVR(ADC1));
  DONE();
}

void watchdog(void)
{
  enable();
  LL_ADC_REG_SetSequencerRanks(ADC1, LL_ADC_REG_RANK_1, LL_ADC_CHANNEL_5);
  LL_ADC_SetAnalogWDThresholds(ADC1, LL_ADC_AWD_THRESHOLD_HIGH, 3000u);
  LL_ADC_SetAnalogWDThresholds(ADC1, LL_ADC_AWD_THRESHOLD_LOW, 1000u);
  LL_ADC_SetAnalogWDMonitChannels(ADC1, LL_ADC_AWD_CHANNEL_5_REG);
  LOG(convert());
  LOG(LL_ADC_IsActiveFlag_AWD1(ADC1));
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
