#include "periph_test.h"
#include "stm32l4xx.h"
#include "stm32l4xx_ll_adc.h"
#include "stm32l4xx_ll_bus.h"
#include "stm32l4xx_ll_dma.h"
#include "stm32l4xx_ll_gpio.h"
#include "stm32l4xx_ll_rcc.h"

#define REGULATOR_SPIN   200u
#define CALIBRATION_SPIN 32u
#define SEQUENCE_SPIN    1000u
#define SCAN_RANKS       3u

uint16_t scan[SCAN_RANKS];

void idle(void);
void aden_at_once(void);
void injected(void);
void auto_injected(void);
void dma_scan(void);

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

void injected(void)
{
  calibrate();
  spin(CALIBRATION_SPIN);
  LL_ADC_Enable(ADC1);
  while(LL_ADC_IsActiveFlag_ADRDY(ADC1) == 0u)
  {
  }
  LL_ADC_INJ_SetTriggerSource(ADC1, LL_ADC_INJ_TRIG_SOFTWARE);
  LL_ADC_INJ_SetSequencerRanks(ADC1, LL_ADC_INJ_RANK_1, LL_ADC_CHANNEL_5);
  LL_ADC_INJ_StartConversion(ADC1);
  while(LL_ADC_IsActiveFlag_JEOS(ADC1) == 0u)
  {
  }
  LOG(LL_ADC_INJ_ReadConversionData12(ADC1, LL_ADC_INJ_RANK_1));
  LOG(LL_ADC_IsActiveFlag_JEOC(ADC1));
  LOG(LL_ADC_INJ_IsConversionOngoing(ADC1));
  DONE();
}

void auto_injected(void)
{
  calibrate();
  spin(CALIBRATION_SPIN);
  LL_ADC_Enable(ADC1);
  while(LL_ADC_IsActiveFlag_ADRDY(ADC1) == 0u)
  {
  }
  LL_ADC_REG_SetSequencerRanks(ADC1, LL_ADC_REG_RANK_1, LL_ADC_CHANNEL_5);
  LL_ADC_INJ_SetTriggerSource(ADC1, LL_ADC_INJ_TRIG_SOFTWARE);
  LL_ADC_INJ_SetSequencerRanks(ADC1, LL_ADC_INJ_RANK_1, LL_ADC_CHANNEL_VREFINT);
  LL_ADC_INJ_SetTrigAuto(ADC1, LL_ADC_INJ_TRIG_FROM_GRP_REGULAR);
  LL_ADC_REG_StartConversion(ADC1);
  while(LL_ADC_IsActiveFlag_JEOS(ADC1) == 0u)
  {
  }
  LOG(LL_ADC_REG_ReadConversionData12(ADC1));
  LOG(LL_ADC_INJ_ReadConversionData12(ADC1, LL_ADC_INJ_RANK_1));
  LOG(LL_ADC_REG_IsConversionOngoing(ADC1));
  LOG(LL_ADC_INJ_IsConversionOngoing(ADC1));
  DONE();
}

void dma_scan(void)
{
  calibrate();
  spin(CALIBRATION_SPIN);
  LL_ADC_Enable(ADC1);
  while(LL_ADC_IsActiveFlag_ADRDY(ADC1) == 0u)
  {
  }
  LL_AHB1_GRP1_EnableClock(LL_AHB1_GRP1_PERIPH_DMA1);
  LL_DMA_SetPeriphRequest(DMA1, LL_DMA_CHANNEL_1, LL_DMA_REQUEST_0);
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
      (uint32_t)scan, LL_DMA_DIRECTION_PERIPH_TO_MEMORY);
  LL_DMA_SetDataLength(DMA1, LL_DMA_CHANNEL_1, SCAN_RANKS);
  LL_DMA_EnableChannel(DMA1, LL_DMA_CHANNEL_1);
  LL_ADC_REG_SetSequencerLength(ADC1, LL_ADC_REG_SEQ_SCAN_ENABLE_3RANKS);
  LL_ADC_REG_SetSequencerRanks(ADC1, LL_ADC_REG_RANK_1, LL_ADC_CHANNEL_5);
  LL_ADC_REG_SetSequencerRanks(ADC1, LL_ADC_REG_RANK_2, LL_ADC_CHANNEL_VREFINT);
  LL_ADC_REG_SetSequencerRanks(ADC1, LL_ADC_REG_RANK_3, LL_ADC_CHANNEL_TEMPSENSOR);
  LL_ADC_REG_SetContinuousMode(ADC1, LL_ADC_REG_CONV_CONTINUOUS);
  LL_ADC_REG_SetDMATransfer(ADC1, LL_ADC_REG_DMA_TRANSFER_LIMITED);
  LL_ADC_REG_StartConversion(ADC1);
  while(LL_DMA_IsActiveFlag_TC1(DMA1) == 0u)
  {
  }
  LL_ADC_ClearFlag_EOS(ADC1);
  spin(SEQUENCE_SPIN);
  for(uint32_t rank = 0; rank < SCAN_RANKS; ++rank)
  {
    LOG(scan[rank]);
  }
  LOG(LL_DMA_GetDataLength(DMA1, LL_DMA_CHANNEL_1));
  LOG(LL_ADC_REG_IsConversionOngoing(ADC1));
  LOG(LL_ADC_IsActiveFlag_EOS(ADC1));
  LOG(LL_ADC_IsActiveFlag_OVR(ADC1));
  LL_ADC_REG_StopConversion(ADC1);
  while(LL_ADC_REG_IsConversionOngoing(ADC1) != 0u)
  {
  }
  LOG(LL_ADC_REG_IsStopConversionOngoing(ADC1));
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
