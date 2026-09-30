#include "main.h"

#include "FreeRTOS.h"
#include "task.h"

#include "stm32u5xx_ll_spi.h"

void SPI1_IRQHandler(void);

static struct
{
  uint8_t const* out;
  uint8_t*       in;
  uint32_t       size;
  uint32_t       sent;
  uint32_t       received;
  TaskHandle_t   waiter;
} volatile transfer;

void spi_exchange(uint8_t const* out, uint8_t* in, uint32_t size)
{
  taskENTER_CRITICAL();
  transfer.out      = out;
  transfer.in       = in;
  transfer.size     = size;
  transfer.sent     = 0;
  transfer.received = 0;
  transfer.waiter   = xTaskGetCurrentTaskHandle();
  LL_SPI_SetTransferSize(SPI1, size);
  LL_SPI_EnableIT_RXP(SPI1);
  LL_SPI_EnableIT_TXP(SPI1);
  LL_SPI_EnableIT_EOT(SPI1);
  taskEXIT_CRITICAL();
  LL_SPI_Enable(SPI1);
  LL_SPI_StartMasterTransfer(SPI1);
  ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
}

void SPI1_IRQHandler(void)
{
  if(LL_SPI_IsActiveFlag_RXP(SPI1) && transfer.received < transfer.size)
  {
    transfer.in[transfer.received++] = LL_SPI_ReceiveData8(SPI1);
  }
  if(LL_SPI_IsEnabledIT_TXP(SPI1) && LL_SPI_IsActiveFlag_TXP(SPI1))
  {
    LL_SPI_TransmitData8(SPI1, transfer.out[transfer.sent++]);
    if(transfer.sent == transfer.size)
    {
      LL_SPI_DisableIT_TXP(SPI1);
    }
  }
  if(LL_SPI_IsActiveFlag_EOT(SPI1))
  {
    while(LL_SPI_IsActiveFlag_RXP(SPI1) && transfer.received < transfer.size)
    {
      transfer.in[transfer.received++] = LL_SPI_ReceiveData8(SPI1);
    }
    LL_SPI_ClearFlag_EOT(SPI1);
    LL_SPI_ClearFlag_TXTF(SPI1);
    LL_SPI_DisableIT_EOT(SPI1);
    LL_SPI_DisableIT_RXP(SPI1);
    LL_SPI_Disable(SPI1);
    BaseType_t woken = pdFALSE;
    vTaskNotifyGiveFromISR(transfer.waiter, &woken);
    portYIELD_FROM_ISR(woken);
  }
}
