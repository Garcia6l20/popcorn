#include "main.h"

#include "FreeRTOS.h"
#include "task.h"

#include "stm32h5xx_ll_bus.h"
#include "stm32h5xx_ll_dma.h"
#include "stm32h5xx_ll_spi.h"

#define TX_CHANNEL LL_DMA_CHANNEL_0
#define RX_CHANNEL LL_DMA_CHANNEL_1

void GPDMA1_Channel1_IRQHandler(void);

static TaskHandle_t volatile waiter;

static void dma_channel(
    uint32_t channel,
    uint32_t request,
    uint32_t direction,
    uint32_t increment,
    uint32_t source,
    uint32_t destination,
    uint32_t size)
{
  LL_DMA_ConfigTransfer(
      GPDMA1, channel, increment | LL_DMA_SRC_DATAWIDTH_BYTE | LL_DMA_DEST_DATAWIDTH_BYTE);
  LL_DMA_SetDataTransferDirection(GPDMA1, channel, direction);
  LL_DMA_SetPeriphRequest(GPDMA1, channel, request);
  LL_DMA_SetBlkDataLength(GPDMA1, channel, size);
  LL_DMA_SetSrcAddress(GPDMA1, channel, source);
  LL_DMA_SetDestAddress(GPDMA1, channel, destination);
  LL_DMA_ClearFlag_TC(GPDMA1, channel);
}

void spi_exchange(uint8_t const* out, uint8_t* in, uint32_t size)
{
  waiter = xTaskGetCurrentTaskHandle();
  LL_AHB1_GRP1_EnableClock(LL_AHB1_GRP1_PERIPH_GPDMA1);
  dma_channel(
      TX_CHANNEL, LL_GPDMA1_REQUEST_SPI4_TX, LL_DMA_DIRECTION_MEMORY_TO_PERIPH,
      LL_DMA_SRC_INCREMENT | LL_DMA_DEST_FIXED, (uint32_t)out, (uint32_t)&SPI4->TXDR, size);
  dma_channel(
      RX_CHANNEL, LL_GPDMA1_REQUEST_SPI4_RX, LL_DMA_DIRECTION_PERIPH_TO_MEMORY,
      LL_DMA_SRC_FIXED | LL_DMA_DEST_INCREMENT, (uint32_t)&SPI4->RXDR, (uint32_t)in, size);
  LL_DMA_EnableIT_TC(GPDMA1, RX_CHANNEL);
  NVIC_SetPriority(GPDMA1_Channel1_IRQn, 5);
  NVIC_EnableIRQ(GPDMA1_Channel1_IRQn);
  LL_SPI_SetTransferSize(SPI4, size);
  LL_SPI_EnableDMAReq_RX(SPI4);
  LL_DMA_EnableChannel(GPDMA1, RX_CHANNEL);
  LL_DMA_EnableChannel(GPDMA1, TX_CHANNEL);
  LL_SPI_EnableDMAReq_TX(SPI4);
  LL_SPI_Enable(SPI4);
  LL_SPI_StartMasterTransfer(SPI4);
  ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
  while(!LL_SPI_IsActiveFlag_EOT(SPI4))
  {
  }
  LL_SPI_ClearFlag_EOT(SPI4);
  LL_SPI_ClearFlag_TXTF(SPI4);
  LL_SPI_Disable(SPI4);
  LL_SPI_DisableDMAReq_TX(SPI4);
  LL_SPI_DisableDMAReq_RX(SPI4);
}

void GPDMA1_Channel1_IRQHandler(void)
{
  if(LL_DMA_IsActiveFlag_TC(GPDMA1, RX_CHANNEL))
  {
    LL_DMA_ClearFlag_TC(GPDMA1, RX_CHANNEL);
    BaseType_t woken = pdFALSE;
    vTaskNotifyGiveFromISR(waiter, &woken);
    portYIELD_FROM_ISR(woken);
  }
}
