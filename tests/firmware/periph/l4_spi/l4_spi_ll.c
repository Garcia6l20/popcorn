#include "periph_test.h"
#include "stm32l4xx_hal.h"
#include "stm32l4xx_ll_bus.h"
#include "stm32l4xx_ll_spi.h"

static uint8_t const     digits[] __attribute__((aligned(4))) = "123456789";
static uint8_t           received[16] __attribute__((aligned(4)));
static volatile uint32_t irq_next;
static volatile uint32_t irq_got;
static SPI_HandleTypeDef spi;
static DMA_HandleTypeDef dma_rx;
static DMA_HandleTypeDef dma_tx;

void idle(void);
void master8(void);
void ds16(void);
void ds4(void);
void packed16(void);
void odd_packed(void);
void crc8(void);
void crc16_ds8(void);
void overrun(void);
void rx_only(void);
void irq(void);
void hal_txrx(void);
void hal_txrx_dma_odd(void);
void hal_crc(void);
void SPI1_IRQHandler(void);
void DMA1_Channel2_IRQHandler(void);
void DMA1_Channel3_IRQHandler(void);

void idle(void)
{
  for(;;)
  {
    __WFI();
  }
}

static void start(uint32_t width, uint32_t threshold)
{
  LL_APB2_GRP1_EnableClock(LL_APB2_GRP1_PERIPH_SPI1);
  LL_SPI_SetMode(SPI1, LL_SPI_MODE_MASTER);
  LL_SPI_SetBaudRatePrescaler(SPI1, LL_SPI_BAUDRATEPRESCALER_DIV4);
  LL_SPI_SetDataWidth(SPI1, width);
  LL_SPI_SetRxFIFOThreshold(SPI1, threshold);
  LL_SPI_SetNSSMode(SPI1, LL_SPI_NSS_HARD_OUTPUT);
}

static void wait_txe(void)
{
  while(!LL_SPI_IsActiveFlag_TXE(SPI1))
  {
  }
}

static void wait_rxne(void)
{
  while(!LL_SPI_IsActiveFlag_RXNE(SPI1))
  {
  }
}

static uint32_t exchange8(uint8_t value)
{
  wait_txe();
  LL_SPI_TransmitData8(SPI1, value);
  wait_rxne();
  return LL_SPI_ReceiveData8(SPI1);
}

static void finish(void)
{
  while(LL_SPI_GetTxFIFOLevel(SPI1) != LL_SPI_TX_FIFO_EMPTY)
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
  start(LL_SPI_DATAWIDTH_8BIT, LL_SPI_RX_FIFO_TH_QUARTER);
  LL_SPI_Enable(SPI1);
  static uint8_t const out[] = {0x11, 0x22, 0x33, 0x44};
  for(uint32_t index = 0; index < sizeof out; ++index)
  {
    LOG(exchange8(out[index]));
  }
  finish();
  DONE();
}

void ds16(void)
{
  start(LL_SPI_DATAWIDTH_16BIT, LL_SPI_RX_FIFO_TH_HALF);
  LL_SPI_Enable(SPI1);
  static uint16_t const out[] = {0x1234, 0xABCD};
  for(uint32_t index = 0; index < 2u; ++index)
  {
    wait_txe();
    LL_SPI_TransmitData16(SPI1, out[index]);
    wait_rxne();
    LOG(LL_SPI_ReceiveData16(SPI1));
  }
  finish();
  DONE();
}

void ds4(void)
{
  start(LL_SPI_DATAWIDTH_4BIT, LL_SPI_RX_FIFO_TH_QUARTER);
  LL_SPI_Enable(SPI1);
  static uint8_t const out[] = {0x03, 0x0C, 0x35};
  for(uint32_t index = 0; index < sizeof out; ++index)
  {
    LOG(exchange8(out[index]));
  }
  finish();
  DONE();
}

void packed16(void)
{
  start(LL_SPI_DATAWIDTH_8BIT, LL_SPI_RX_FIFO_TH_HALF);
  LL_SPI_Enable(SPI1);
  static uint16_t const out[] = {0x2211, 0x4433};
  for(uint32_t index = 0; index < 2u; ++index)
  {
    wait_txe();
    LL_SPI_TransmitData16(SPI1, out[index]);
    wait_rxne();
    LOG(LL_SPI_ReceiveData16(SPI1));
  }
  finish();
  DONE();
}

void odd_packed(void)
{
  start(LL_SPI_DATAWIDTH_8BIT, LL_SPI_RX_FIFO_TH_HALF);
  LL_SPI_Enable(SPI1);
  LL_SPI_TransmitData16(SPI1, 0x2211);
  LL_SPI_TransmitData8(SPI1, 0x33);
  wait_rxne();
  LOG(LL_SPI_ReceiveData16(SPI1));
  LL_SPI_SetRxFIFOThreshold(SPI1, LL_SPI_RX_FIFO_TH_QUARTER);
  wait_rxne();
  LOG(LL_SPI_ReceiveData8(SPI1));
  finish();
  DONE();
}

static void crc_tail(uint32_t frames)
{
  for(uint32_t index = 0; index < frames; ++index)
  {
    wait_rxne();
    LOG(LL_SPI_ReceiveData8(SPI1));
  }
  LOG(LL_SPI_GetTxCRC(SPI1));
  LOG(LL_SPI_GetRxCRC(SPI1));
  LOG(LL_SPI_IsActiveFlag_CRCERR(SPI1));
  finish();
}

static void crc_body(void)
{
  for(uint32_t index = 0; index < 9u; ++index)
  {
    wait_txe();
    LL_SPI_TransmitData8(SPI1, digits[index]);
    if(index == 8u)
    {
      LL_SPI_SetCRCNext(SPI1);
    }
    wait_rxne();
    received[index] = LL_SPI_ReceiveData8(SPI1);
  }
}

void crc8(void)
{
  start(LL_SPI_DATAWIDTH_8BIT, LL_SPI_RX_FIFO_TH_QUARTER);
  LL_SPI_EnableCRC(SPI1);
  LL_SPI_Enable(SPI1);
  crc_body();
  crc_tail(1u);
  DONE();
}

void crc16_ds8(void)
{
  start(LL_SPI_DATAWIDTH_8BIT, LL_SPI_RX_FIFO_TH_QUARTER);
  LL_SPI_SetCRCWidth(SPI1, LL_SPI_CRC_16BIT);
  LL_SPI_SetCRCPolynomial(SPI1, 0x1021u);
  LL_SPI_EnableCRC(SPI1);
  LL_SPI_Enable(SPI1);
  crc_body();
  crc_tail(2u);
  DONE();
}

void overrun(void)
{
  start(LL_SPI_DATAWIDTH_8BIT, LL_SPI_RX_FIFO_TH_QUARTER);
  LL_SPI_Enable(SPI1);
  for(uint32_t index = 0; index < 5u; ++index)
  {
    wait_txe();
    LL_SPI_TransmitData8(SPI1, digits[index]);
  }
  while(LL_SPI_IsActiveFlag_BSY(SPI1))
  {
  }
  LOG(LL_SPI_IsActiveFlag_OVR(SPI1));
  LOG(LL_SPI_GetRxFIFOLevel(SPI1));
  for(uint32_t index = 0; index < 4u; ++index)
  {
    LOG(LL_SPI_ReceiveData8(SPI1));
  }
  LL_SPI_ClearFlag_OVR(SPI1);
  LOG(SPI1->SR);
  LL_SPI_Disable(SPI1);
  DONE();
}

void rx_only(void)
{
  start(LL_SPI_DATAWIDTH_8BIT, LL_SPI_RX_FIFO_TH_QUARTER);
  LL_SPI_SetTransferDirection(SPI1, LL_SPI_SIMPLEX_RX);
  LL_SPI_Enable(SPI1);
  for(uint32_t index = 0; index < 3u; ++index)
  {
    wait_rxne();
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
  if(spi.Instance != NULL)
  {
    HAL_SPI_IRQHandler(&spi);
    return;
  }
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
  start(LL_SPI_DATAWIDTH_8BIT, LL_SPI_RX_FIFO_TH_QUARTER);
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

static void hal_init(uint32_t crc)
{
  __HAL_RCC_SPI1_CLK_ENABLE();
  spi.Instance               = SPI1;
  spi.Init.Mode              = SPI_MODE_MASTER;
  spi.Init.Direction         = SPI_DIRECTION_2LINES;
  spi.Init.DataSize          = SPI_DATASIZE_8BIT;
  spi.Init.CLKPolarity       = SPI_POLARITY_LOW;
  spi.Init.CLKPhase          = SPI_PHASE_1EDGE;
  spi.Init.NSS               = SPI_NSS_HARD_OUTPUT;
  spi.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_4;
  spi.Init.FirstBit          = SPI_FIRSTBIT_MSB;
  spi.Init.TIMode            = SPI_TIMODE_DISABLE;
  spi.Init.CRCCalculation    = crc;
  spi.Init.CRCPolynomial     = 7;
  spi.Init.CRCLength         = SPI_CRC_LENGTH_DATASIZE;
  spi.Init.NSSPMode          = SPI_NSS_PULSE_DISABLE;
  LOG(HAL_SPI_Init(&spi));
}

static void log_received(uint32_t count)
{
  for(uint32_t index = 0; index < count; ++index)
  {
    LOG(received[index]);
  }
}

void hal_txrx(void)
{
  hal_init(SPI_CRCCALCULATION_DISABLE);
  LOG(HAL_SPI_TransmitReceive(&spi, (uint8_t*)digits, received, 7u, 1000u));
  log_received(7u);
  LOG(SPI1->SR);
  DONE();
}

static void dma_channel(
    DMA_HandleTypeDef*   handle,
    DMA_Channel_TypeDef* channel,
    uint32_t             direction,
    IRQn_Type            line)
{
  handle->Instance                 = channel;
  handle->Init.Request             = DMA_REQUEST_1;
  handle->Init.Direction           = direction;
  handle->Init.PeriphInc           = DMA_PINC_DISABLE;
  handle->Init.MemInc              = DMA_MINC_ENABLE;
  handle->Init.PeriphDataAlignment = DMA_PDATAALIGN_HALFWORD;
  handle->Init.MemDataAlignment    = DMA_MDATAALIGN_HALFWORD;
  handle->Init.Mode                = DMA_NORMAL;
  handle->Init.Priority            = DMA_PRIORITY_LOW;
  LOG(HAL_DMA_Init(handle));
  HAL_NVIC_EnableIRQ(line);
}

void DMA1_Channel2_IRQHandler(void)
{
  HAL_DMA_IRQHandler(&dma_rx);
}

void DMA1_Channel3_IRQHandler(void)
{
  HAL_DMA_IRQHandler(&dma_tx);
}

void hal_txrx_dma_odd(void)
{
  __HAL_RCC_DMA1_CLK_ENABLE();
  hal_init(SPI_CRCCALCULATION_DISABLE);
  dma_channel(&dma_rx, DMA1_Channel2, DMA_PERIPH_TO_MEMORY, DMA1_Channel2_IRQn);
  dma_channel(&dma_tx, DMA1_Channel3, DMA_MEMORY_TO_PERIPH, DMA1_Channel3_IRQn);
  __HAL_LINKDMA(&spi, hdmarx, dma_rx);
  __HAL_LINKDMA(&spi, hdmatx, dma_tx);
  HAL_NVIC_EnableIRQ(SPI1_IRQn);
  LOG(HAL_SPI_TransmitReceive_DMA(&spi, (uint8_t*)digits, received, 7u));
  LOG(SPI1->CR2);
  while(HAL_SPI_GetState(&spi) != HAL_SPI_STATE_READY)
  {
  }
  LOG(spi.ErrorCode);
  log_received(8u);
  LOG(SPI1->SR);
  DONE();
}

void hal_crc(void)
{
  hal_init(SPI_CRCCALCULATION_ENABLE);
  LOG(HAL_SPI_TransmitReceive(&spi, (uint8_t*)digits, received, 9u, 1000u));
  LOG(spi.ErrorCode);
  LOG(SPI1->TXCRCR);
  LOG(SPI1->RXCRCR);
  LOG(SPI1->SR);
  DONE();
}

int main(void)
{
  DONE();
}
