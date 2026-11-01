#include "periph_test.h"
#include "stm32l1xx.h"
#include "stm32l1xx_ll_bus.h"
#include "stm32l1xx_ll_dma.h"
#include "stm32l1xx_ll_spi.h"

static uint8_t const     digits[] = "123456789";
static uint8_t           received[16];
static volatile uint32_t irq_next;
static volatile uint32_t irq_got;

void idle(void);
void master8(void);
void master16(void);
void crc8(void);
void crc16(void);
void dma_crc(void);
void overrun(void);
void rx_only(void);
void irq(void);
void SPI1_IRQHandler(void);

void idle(void)
{
  for(;;)
  {
    __WFI();
  }
}

static void start(uint32_t width)
{
  LL_APB2_GRP1_EnableClock(LL_APB2_GRP1_PERIPH_SPI1);
  LL_SPI_SetMode(SPI1, LL_SPI_MODE_MASTER);
  LL_SPI_SetBaudRatePrescaler(SPI1, LL_SPI_BAUDRATEPRESCALER_DIV4);
  LL_SPI_SetDataWidth(SPI1, width);
  LL_SPI_SetNSSMode(SPI1, LL_SPI_NSS_HARD_OUTPUT);
}

static uint32_t exchange8(uint8_t value)
{
  while(!LL_SPI_IsActiveFlag_TXE(SPI1))
  {
  }
  LL_SPI_TransmitData8(SPI1, value);
  while(!LL_SPI_IsActiveFlag_RXNE(SPI1))
  {
  }
  return LL_SPI_ReceiveData8(SPI1);
}

static void finish(void)
{
  while(!LL_SPI_IsActiveFlag_TXE(SPI1))
  {
  }
  while(LL_SPI_IsActiveFlag_BSY(SPI1))
  {
  }
  LOG(SPI1->SR);
  LL_SPI_Disable(SPI1);
}

void master8(void)
{
  start(LL_SPI_DATAWIDTH_8BIT);
  LL_SPI_Enable(SPI1);
  static uint8_t const out[] = {0x11, 0x22, 0x33, 0x44};
  for(uint32_t index = 0; index < sizeof out; ++index)
  {
    LOG(exchange8(out[index]));
  }
  finish();
  DONE();
}

void master16(void)
{
  start(LL_SPI_DATAWIDTH_16BIT);
  LL_SPI_Enable(SPI1);
  static uint16_t const out[] = {0x1234, 0xABCD};
  for(uint32_t index = 0; index < 2u; ++index)
  {
    while(!LL_SPI_IsActiveFlag_TXE(SPI1))
    {
    }
    LL_SPI_TransmitData16(SPI1, out[index]);
    while(!LL_SPI_IsActiveFlag_RXNE(SPI1))
    {
    }
    LOG(LL_SPI_ReceiveData16(SPI1));
  }
  finish();
  DONE();
}

static void wait_crc(void)
{
  while(!LL_SPI_IsActiveFlag_RXNE(SPI1))
  {
  }
  LOG(SPI1->DR);
  LOG(LL_SPI_GetTxCRC(SPI1));
  LOG(LL_SPI_GetRxCRC(SPI1));
  LOG(LL_SPI_IsActiveFlag_CRCERR(SPI1));
  finish();
}

void crc8(void)
{
  start(LL_SPI_DATAWIDTH_8BIT);
  LL_SPI_EnableCRC(SPI1);
  LL_SPI_Enable(SPI1);
  for(uint32_t index = 0; index < 9u; ++index)
  {
    while(!LL_SPI_IsActiveFlag_TXE(SPI1))
    {
    }
    LL_SPI_TransmitData8(SPI1, digits[index]);
    if(index == 8u)
    {
      LL_SPI_SetCRCNext(SPI1);
    }
    while(!LL_SPI_IsActiveFlag_RXNE(SPI1))
    {
    }
    received[index] = LL_SPI_ReceiveData8(SPI1);
  }
  wait_crc();
  DONE();
}

void crc16(void)
{
  start(LL_SPI_DATAWIDTH_16BIT);
  LL_SPI_SetCRCPolynomial(SPI1, 0x1021u);
  LL_SPI_EnableCRC(SPI1);
  LL_SPI_Enable(SPI1);
  for(uint32_t index = 0; index < 4u; ++index)
  {
    while(!LL_SPI_IsActiveFlag_TXE(SPI1))
    {
    }
    LL_SPI_TransmitData16(SPI1, (uint16_t)(digits[2u * index] << 8 | digits[2u * index + 1u]));
    if(index == 3u)
    {
      LL_SPI_SetCRCNext(SPI1);
    }
    while(!LL_SPI_IsActiveFlag_RXNE(SPI1))
    {
    }
    (void)LL_SPI_ReceiveData16(SPI1);
  }
  wait_crc();
  DONE();
}

static void dma_channel(uint32_t channel, uint32_t direction, uint32_t memory, uint32_t items)
{
  LL_DMA_ConfigTransfer(
      DMA1, channel,
      direction
          | LL_DMA_MODE_NORMAL
          | LL_DMA_PERIPH_NOINCREMENT
          | LL_DMA_MEMORY_INCREMENT
          | LL_DMA_PDATAALIGN_BYTE
          | LL_DMA_MDATAALIGN_BYTE
          | LL_DMA_PRIORITY_LOW);
  uint32_t const peripheral = LL_SPI_DMA_GetRegAddr(SPI1);
  if(direction == LL_DMA_DIRECTION_PERIPH_TO_MEMORY)
  {
    LL_DMA_ConfigAddresses(DMA1, channel, peripheral, memory, direction);
  }
  else
  {
    LL_DMA_ConfigAddresses(DMA1, channel, memory, peripheral, direction);
  }
  LL_DMA_SetDataLength(DMA1, channel, items);
  LL_DMA_EnableChannel(DMA1, channel);
}

void dma_crc(void)
{
  LL_AHB1_GRP1_EnableClock(LL_AHB1_GRP1_PERIPH_DMA1);
  start(LL_SPI_DATAWIDTH_8BIT);
  LL_SPI_EnableCRC(SPI1);
  dma_channel(LL_DMA_CHANNEL_2, LL_DMA_DIRECTION_PERIPH_TO_MEMORY, (uint32_t)received, 9u);
  dma_channel(LL_DMA_CHANNEL_3, LL_DMA_DIRECTION_MEMORY_TO_PERIPH, (uint32_t)digits, 9u);
  LL_SPI_EnableDMAReq_RX(SPI1);
  LL_SPI_Enable(SPI1);
  LL_SPI_EnableDMAReq_TX(SPI1);
  while(LL_DMA_IsActiveFlag_TC2(DMA1) == 0u)
  {
  }
  LL_SPI_DisableDMAReq_RX(SPI1);
  LOG(received[0] | received[8] << 8);
  wait_crc();
  DONE();
}

void overrun(void)
{
  start(LL_SPI_DATAWIDTH_8BIT);
  LL_SPI_Enable(SPI1);
  LL_SPI_TransmitData8(SPI1, 0x5A);
  while(!LL_SPI_IsActiveFlag_TXE(SPI1))
  {
  }
  LL_SPI_TransmitData8(SPI1, 0xA5);
  while(LL_SPI_IsActiveFlag_BSY(SPI1))
  {
  }
  LOG(LL_SPI_IsActiveFlag_OVR(SPI1));
  LOG(LL_SPI_ReceiveData8(SPI1));
  LOG(LL_SPI_IsActiveFlag_OVR(SPI1));
  LL_SPI_ClearFlag_OVR(SPI1);
  LOG(SPI1->SR);
  LL_SPI_Disable(SPI1);
  DONE();
}

void rx_only(void)
{
  start(LL_SPI_DATAWIDTH_8BIT);
  LL_SPI_SetTransferDirection(SPI1, LL_SPI_SIMPLEX_RX);
  LL_SPI_Enable(SPI1);
  for(uint32_t index = 0; index < 3u; ++index)
  {
    while(!LL_SPI_IsActiveFlag_RXNE(SPI1))
    {
    }
    LOG(LL_SPI_ReceiveData8(SPI1));
    if(index == 1u)
    {
      LL_SPI_Disable(SPI1);
    }
  }
  LOG(LL_SPI_IsActiveFlag_BSY(SPI1));
  DONE();
}

void SPI1_IRQHandler(void)
{
  if(LL_SPI_IsActiveFlag_RXNE(SPI1))
  {
    received[irq_got++] = LL_SPI_ReceiveData8(SPI1);
  }
  if(LL_SPI_IsActiveFlag_TXE(SPI1) && LL_SPI_IsEnabledIT_TXE(SPI1))
  {
    LL_SPI_TransmitData8(SPI1, digits[irq_next++]);
    if(irq_next == 4u)
    {
      LL_SPI_DisableIT_TXE(SPI1);
    }
  }
}

void irq(void)
{
  start(LL_SPI_DATAWIDTH_8BIT);
  NVIC_EnableIRQ(SPI1_IRQn);
  LL_SPI_Enable(SPI1);
  LL_SPI_EnableIT_RXNE(SPI1);
  LL_SPI_EnableIT_TXE(SPI1);
  while(irq_got < 4u)
  {
  }
  LOG(received[0] | received[1] << 8 | received[2] << 16 | (uint32_t)received[3] << 24);
  finish();
  DONE();
}

int main(void)
{
  master8();
  return 0;
}
