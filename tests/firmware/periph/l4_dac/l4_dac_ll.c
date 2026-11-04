#include "periph_test.h"
#include "stm32l4xx.h"
#include "stm32l4xx_ll_bus.h"
#include "stm32l4xx_ll_dac.h"
#include "stm32l4xx_ll_dma.h"
#include "stm32l4xx_ll_tim.h"

#define SAMPLES     4u
#define UPDATES     6u
#define SETTLE_SPIN 8u
#define PERIOD      199u

uint32_t wave[SAMPLES] = {0x400u, 0x800u, 0xC00u, 0xFFFu};

void idle(void);

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

int main(void)
{
  LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_DAC1 | LL_APB1_GRP1_PERIPH_TIM6);
  LL_AHB1_GRP1_EnableClock(LL_AHB1_GRP1_PERIPH_DMA1);
  LL_DMA_SetPeriphRequest(DMA1, LL_DMA_CHANNEL_3, LL_DMA_REQUEST_6);
  LL_DMA_ConfigTransfer(
      DMA1, LL_DMA_CHANNEL_3,
      LL_DMA_DIRECTION_MEMORY_TO_PERIPH
          | LL_DMA_MODE_NORMAL
          | LL_DMA_PERIPH_NOINCREMENT
          | LL_DMA_MEMORY_INCREMENT
          | LL_DMA_PDATAALIGN_WORD
          | LL_DMA_MDATAALIGN_WORD
          | LL_DMA_PRIORITY_HIGH);
  LL_DMA_ConfigAddresses(
      DMA1, LL_DMA_CHANNEL_3, (uint32_t)wave,
      LL_DAC_DMA_GetRegAddr(DAC1, LL_DAC_CHANNEL_1, LL_DAC_DMA_REG_DATA_12BITS_RIGHT_ALIGNED),
      LL_DMA_DIRECTION_MEMORY_TO_PERIPH);
  LL_DMA_SetDataLength(DMA1, LL_DMA_CHANNEL_3, SAMPLES);
  LL_DMA_EnableChannel(DMA1, LL_DMA_CHANNEL_3);
  LL_DAC_SetTriggerSource(DAC1, LL_DAC_CHANNEL_1, LL_DAC_TRIG_EXT_TIM6_TRGO);
  LL_DAC_EnableTrigger(DAC1, LL_DAC_CHANNEL_1);
  LL_DAC_EnableDMAReq(DAC1, LL_DAC_CHANNEL_1);
  LL_DAC_ConvertData12RightAligned(DAC1, LL_DAC_CHANNEL_1, 0x100u);
  LL_DAC_Enable(DAC1, LL_DAC_CHANNEL_1);
  LL_TIM_SetPrescaler(TIM6, 0u);
  LL_TIM_SetAutoReload(TIM6, PERIOD);
  LL_TIM_SetTriggerOutput(TIM6, LL_TIM_TRGO_UPDATE);
  LL_TIM_EnableCounter(TIM6);
  for(uint32_t update = 0; update < UPDATES; ++update)
  {
    while(LL_TIM_IsActiveFlag_UPDATE(TIM6) == 0u)
    {
    }
    LL_TIM_ClearFlag_UPDATE(TIM6);
    spin(SETTLE_SPIN);
    LOG(LL_DAC_RetrieveOutputData(DAC1, LL_DAC_CHANNEL_1));
  }
  LOG(LL_DMA_IsActiveFlag_TC3(DMA1));
  LOG(LL_DMA_GetDataLength(DMA1, LL_DMA_CHANNEL_3));
  LOG(LL_DAC_IsActiveFlag_DMAUDR1(DAC1));
  DONE();
}
