#include "periph_test.h"
#include "stm32u5xx_ll_bus.h"
#include "stm32u5xx_ll_dma.h"
#include "stm32u5xx_ll_lpuart.h"
#include "stm32u5xx_ll_pwr.h"
#include "stm32u5xx_ll_rcc.h"
#include "stm32u5xx_ll_usart.h"

static char const hello[] = "hello\r\n";

uint8_t                  received[16];
static volatile uint32_t received_count;
static volatile uint32_t tx_next;
static volatile uint32_t tx_refills;

void USART1_IRQHandler(void);
void tx_poll(void);
void tx_irq(void);
void rx_irq(void);
void loop_fifo(void);
void overrun(void);
void uart4_poll(void);
void lpuart_poll(void);
void idle(void);
void tx_fifo_irq(void);
void tx_dma(void);
void GPDMA1_Channel0_IRQHandler(void);

void idle(void)
{
  for(;;)
  {
    __WFI();
  }
}

static void init(USART_TypeDef* usart, uint32_t baud)
{
  LL_USART_InitTypeDef config = {
      .PrescalerValue      = LL_USART_PRESCALER_DIV1,
      .BaudRate            = baud,
      .DataWidth           = LL_USART_DATAWIDTH_8B,
      .StopBits            = LL_USART_STOPBITS_1,
      .Parity              = LL_USART_PARITY_NONE,
      .TransferDirection   = LL_USART_DIRECTION_TX_RX,
      .HardwareFlowControl = LL_USART_HWCONTROL_NONE,
      .OverSampling        = LL_USART_OVERSAMPLING_16,
  };
  LL_USART_Init(usart, &config);
  LL_USART_ConfigAsyncMode(usart);
}

static void start_usart1(void)
{
  LL_APB2_GRP1_EnableClock(LL_APB2_GRP1_PERIPH_USART1);
  LL_RCC_SetUSARTClockSource(LL_RCC_USART1_CLKSOURCE_PCLK2);
  init(USART1, 115200);
}

static void enable(USART_TypeDef* usart)
{
  LL_USART_Enable(usart);
  while(!LL_USART_IsActiveFlag_TEACK(usart) || !LL_USART_IsActiveFlag_REACK(usart))
  {
  }
}

static void send_poll(USART_TypeDef* usart)
{
  for(char const* next = hello; *next != 0; ++next)
  {
    while(!LL_USART_IsActiveFlag_TXE_TXFNF(usart))
    {
    }
    LL_USART_TransmitData8(usart, (uint8_t)*next);
  }
  while(!LL_USART_IsActiveFlag_TC(usart))
  {
  }
}

void USART1_IRQHandler(void)
{
  if(LL_USART_IsActiveFlag_RXNE_RXFNE(USART1))
  {
    uint8_t const data = LL_USART_ReceiveData8(USART1);
    if(received_count < sizeof received)
    {
      received[received_count++] = data;
    }
  }
  if(LL_USART_IsEnabledIT_TXFT(USART1) && LL_USART_IsActiveFlag_TXFT(USART1))
  {
    ++tx_refills;
    while(tx_next < 16 && LL_USART_IsActiveFlag_TXE_TXFNF(USART1))
    {
      LL_USART_TransmitData8(USART1, (uint8_t)(0x30u + tx_next++));
    }
    if(tx_next == 16)
    {
      LL_USART_DisableIT_TXFT(USART1);
    }
  }
  if(LL_USART_IsEnabledIT_TXE_TXFNF(USART1) && LL_USART_IsActiveFlag_TXE_TXFNF(USART1))
  {
    ++tx_refills;
    if(hello[tx_next] != 0)
    {
      LL_USART_TransmitData8(USART1, (uint8_t)hello[tx_next++]);
    }
    else
    {
      LL_USART_DisableIT_TXE_TXFNF(USART1);
    }
  }
}

void tx_poll(void)
{
  start_usart1();
  enable(USART1);
  send_poll(USART1);
  LOG(LL_USART_IsActiveFlag_TC(USART1));
  DONE();
}

void tx_irq(void)
{
  start_usart1();
  enable(USART1);
  NVIC_EnableIRQ(USART1_IRQn);
  LL_USART_EnableIT_TXE_TXFNF(USART1);
  while(LL_USART_IsEnabledIT_TXE_TXFNF(USART1))
  {
  }
  while(!LL_USART_IsActiveFlag_TC(USART1))
  {
  }
  LOG(tx_refills);
  DONE();
}

void rx_irq(void)
{
  start_usart1();
  enable(USART1);
  NVIC_EnableIRQ(USART1_IRQn);
  LL_USART_EnableIT_RXNE_RXFNE(USART1);
  __asm__ volatile("bkpt #3" ::: "memory");
  while(received_count < 5)
  {
  }
  for(uint32_t index = 0; index < 5; ++index)
  {
    LOG(received[index]);
  }
  DONE();
}

void loop_fifo(void)
{
  start_usart1();
  LL_USART_EnableFIFO(USART1);
  enable(USART1);
  uint32_t sent  = 0;
  uint32_t count = 0;
  while(count < 16)
  {
    if(sent < 16 && LL_USART_IsActiveFlag_TXE_TXFNF(USART1))
    {
      LL_USART_TransmitData8(USART1, (uint8_t)(0x40u + sent++));
    }
    if(LL_USART_IsActiveFlag_RXNE_RXFNE(USART1))
    {
      received[count++] = LL_USART_ReceiveData8(USART1);
    }
  }
  uint32_t same = 1;
  for(uint32_t index = 0; index < 16; ++index)
  {
    same = same && received[index] == 0x40u + index;
  }
  LOG(count);
  LOG(same);
  DONE();
}

void overrun(void)
{
  start_usart1();
  enable(USART1);
  __asm__ volatile("bkpt #3" ::: "memory");
  while(!LL_USART_IsActiveFlag_ORE(USART1))
  {
  }
  LOG(LL_USART_IsActiveFlag_ORE(USART1));
  LL_USART_ClearFlag_ORE(USART1);
  LOG(LL_USART_IsActiveFlag_ORE(USART1));
  DONE();
}

void uart4_poll(void)
{
  LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_UART4);
  LL_RCC_SetUARTClockSource(LL_RCC_UART4_CLKSOURCE_PCLK1);
  init(UART4, 9600);
  enable(UART4);
  send_poll(UART4);
  DONE();
}

void lpuart_poll(void)
{
  LL_AHB3_GRP1_EnableClock(LL_AHB3_GRP1_PERIPH_PWR);
  LL_PWR_EnableBkUpAccess();
  LL_RCC_LSE_Enable();
  while(!LL_RCC_LSE_IsReady())
  {
  }
  LL_APB3_GRP1_EnableClock(LL_APB3_GRP1_PERIPH_LPUART1);
  LL_RCC_SetLPUARTClockSource(LL_RCC_LPUART1_CLKSOURCE_LSE);
  LL_LPUART_InitTypeDef config = {
      .PrescalerValue      = LL_LPUART_PRESCALER_DIV1,
      .BaudRate            = 9600,
      .DataWidth           = LL_LPUART_DATAWIDTH_8B,
      .StopBits            = LL_LPUART_STOPBITS_1,
      .Parity              = LL_LPUART_PARITY_NONE,
      .TransferDirection   = LL_LPUART_DIRECTION_TX_RX,
      .HardwareFlowControl = LL_LPUART_HWCONTROL_NONE,
  };
  LL_LPUART_Init(LPUART1, &config);
  LL_LPUART_Enable(LPUART1);
  for(char const* next = hello; *next != 0; ++next)
  {
    while(!LL_LPUART_IsActiveFlag_TXE_TXFNF(LPUART1))
    {
    }
    LL_LPUART_TransmitData8(LPUART1, (uint8_t)*next);
  }
  while(!LL_LPUART_IsActiveFlag_TC(LPUART1))
  {
  }
  DONE();
}

void tx_fifo_irq(void)
{
  start_usart1();
  LL_USART_EnableFIFO(USART1);
  LL_USART_SetTXFIFOThreshold(USART1, LL_USART_FIFOTHRESHOLD_1_8);
  enable(USART1);
  NVIC_EnableIRQ(USART1_IRQn);
  LL_USART_EnableIT_TXFT(USART1);
  while(LL_USART_IsEnabledIT_TXFT(USART1))
  {
  }
  while(!LL_USART_IsActiveFlag_TC(USART1))
  {
  }
  LOG(tx_refills);
  DONE();
}

static char const banner[65] = "popcorn usart dma banner, sixty-four bytes long, sent by gpdma!\n";
static volatile uint32_t dma_done;

void GPDMA1_Channel0_IRQHandler(void)
{
  if(LL_DMA_IsActiveFlag_TC(GPDMA1, LL_DMA_CHANNEL_0))
  {
    LL_DMA_ClearFlag_TC(GPDMA1, LL_DMA_CHANNEL_0);
    ++dma_done;
  }
}

void tx_dma(void)
{
  start_usart1();
  enable(USART1);
  LL_AHB1_GRP1_EnableClock(LL_AHB1_GRP1_PERIPH_GPDMA1);
  LL_DMA_ConfigTransfer(
      GPDMA1, LL_DMA_CHANNEL_0,
      LL_DMA_SRC_INCREMENT
          | LL_DMA_DEST_FIXED
          | LL_DMA_SRC_DATAWIDTH_BYTE
          | LL_DMA_DEST_DATAWIDTH_BYTE);
  LL_DMA_SetDataTransferDirection(GPDMA1, LL_DMA_CHANNEL_0, LL_DMA_DIRECTION_MEMORY_TO_PERIPH);
  LL_DMA_SetPeriphRequest(GPDMA1, LL_DMA_CHANNEL_0, LL_GPDMA1_REQUEST_USART1_TX);
  LL_DMA_SetBlkDataLength(GPDMA1, LL_DMA_CHANNEL_0, 64);
  LL_DMA_SetSrcAddress(GPDMA1, LL_DMA_CHANNEL_0, (uint32_t)banner);
  LL_DMA_SetDestAddress(GPDMA1, LL_DMA_CHANNEL_0, (uint32_t)&USART1->TDR);
  LL_DMA_EnableIT_TC(GPDMA1, LL_DMA_CHANNEL_0);
  NVIC_EnableIRQ(GPDMA1_Channel0_IRQn);
  LL_DMA_EnableChannel(GPDMA1, LL_DMA_CHANNEL_0);
  LL_USART_ClearFlag_TC(USART1);
  LL_USART_EnableDMAReq_TX(USART1);
  while(dma_done == 0)
  {
  }
  while(!LL_USART_IsActiveFlag_TC(USART1))
  {
  }
  LOG(dma_done);
  DONE();
}

int main(void)
{
  tx_poll();
}
