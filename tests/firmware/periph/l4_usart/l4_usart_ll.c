#include "periph_test.h"
#include "stm32l4xx.h"
#include "stm32l4xx_ll_bus.h"
#include "stm32l4xx_ll_dma.h"
#include "stm32l4xx_ll_lpuart.h"
#include "stm32l4xx_ll_pwr.h"
#include "stm32l4xx_ll_rcc.h"
#include "stm32l4xx_ll_usart.h"

static char const hello[] = "hello\r\n";

static uint8_t           received[8];
static volatile uint32_t received_count;
static volatile uint32_t tx_next;

void idle(void);
void hsi16_poll(void);
void uart4_tx_irq(void);
void uart4_rx_irq(void);
void lpuart_lse(void);
void lpuart_pclk1(void);
void dma_tx(void);
void dma_rx(void);
void idle_overrun(void);
void UART4_IRQHandler(void);

void idle(void)
{
  for(;;)
  {
    __WFI();
  }
}

static void init(USART_TypeDef* usart, uint32_t kernel_hz)
{
  LL_USART_SetTransferDirection(usart, LL_USART_DIRECTION_TX_RX);
  LL_USART_ConfigCharacter(usart, LL_USART_DATAWIDTH_8B, LL_USART_PARITY_NONE, LL_USART_STOPBITS_1);
  LL_USART_SetOverSampling(usart, LL_USART_OVERSAMPLING_16);
  LL_USART_SetBaudRate(usart, kernel_hz, LL_USART_OVERSAMPLING_16, 115200u);
  LL_USART_ConfigAsyncMode(usart);
  LL_USART_Enable(usart);
  while(!LL_USART_IsActiveFlag_TEACK(usart) || !LL_USART_IsActiveFlag_REACK(usart))
  {
  }
}

static void send_poll(USART_TypeDef* usart)
{
  for(char const* next = hello; *next != 0; ++next)
  {
    while(!LL_USART_IsActiveFlag_TXE(usart))
    {
    }
    LL_USART_TransmitData8(usart, (uint8_t)*next);
  }
  while(!LL_USART_IsActiveFlag_TC(usart))
  {
  }
}

static void start_uart4(void)
{
  LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_UART4);
  SystemCoreClockUpdate();
  init(UART4, SystemCoreClock);
  NVIC_EnableIRQ(UART4_IRQn);
}

void UART4_IRQHandler(void)
{
  if(LL_USART_IsActiveFlag_RXNE(UART4))
  {
    uint8_t const data = LL_USART_ReceiveData8(UART4);
    if(received_count < sizeof received)
    {
      received[received_count++] = data;
    }
  }
  if(LL_USART_IsEnabledIT_TXE(UART4) && LL_USART_IsActiveFlag_TXE(UART4))
  {
    if(hello[tx_next] != 0)
    {
      LL_USART_TransmitData8(UART4, (uint8_t)hello[tx_next++]);
    }
    else
    {
      LL_USART_DisableIT_TXE(UART4);
    }
  }
}

int main(void)
{
  LOG(USART2->ISR);
  LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_USART2);
  SystemCoreClockUpdate();
  init(USART2, SystemCoreClock);
  send_poll(USART2);
  LOG(USART2->BRR);
  LOG(LL_USART_IsActiveFlag_TC(USART2));
  DONE();
}

void hsi16_poll(void)
{
  LL_RCC_HSI_Enable();
  while(!LL_RCC_HSI_IsReady())
  {
  }
  LL_RCC_SetUSARTClockSource(LL_RCC_USART2_CLKSOURCE_HSI);
  LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_USART2);
  init(USART2, HSI_VALUE);
  send_poll(USART2);
  LOG(USART2->BRR);
  DONE();
}

void uart4_tx_irq(void)
{
  start_uart4();
  LL_USART_EnableIT_TXE(UART4);
  while(LL_USART_IsEnabledIT_TXE(UART4))
  {
    __WFI();
  }
  while(!LL_USART_IsActiveFlag_TC(UART4))
  {
  }
  LOG(tx_next);
  DONE();
}

void uart4_rx_irq(void)
{
  start_uart4();
  LL_USART_EnableIT_RXNE(UART4);
  while(received_count < 4u)
  {
    __WFI();
  }
  LOG(received_count);
  for(uint32_t index = 0; index < 4u; ++index)
  {
    LOG(received[index]);
  }
  DONE();
}

static void lpuart_send(uint32_t kernel_hz, uint32_t baud)
{
  LL_APB1_GRP2_EnableClock(LL_APB1_GRP2_PERIPH_LPUART1);
  LL_LPUART_SetTransferDirection(LPUART1, LL_LPUART_DIRECTION_TX_RX);
  LL_LPUART_ConfigCharacter(
      LPUART1, LL_LPUART_DATAWIDTH_8B, LL_LPUART_PARITY_NONE, LL_LPUART_STOPBITS_1);
  LL_LPUART_SetBaudRate(LPUART1, kernel_hz, baud);
  LL_LPUART_Enable(LPUART1);
  for(char const* next = hello; *next != 0; ++next)
  {
    while(!LL_LPUART_IsActiveFlag_TXE(LPUART1))
    {
    }
    LL_LPUART_TransmitData8(LPUART1, (uint8_t)*next);
  }
  while(!LL_LPUART_IsActiveFlag_TC(LPUART1))
  {
  }
  LOG(LPUART1->BRR);
}

void lpuart_lse(void)
{
  LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_PWR);
  LL_PWR_EnableBkUpAccess();
  LL_RCC_LSE_Enable();
  while(!LL_RCC_LSE_IsReady())
  {
  }
  LL_RCC_SetLPUARTClockSource(LL_RCC_LPUART1_CLKSOURCE_LSE);
  lpuart_send(LSE_VALUE, 9600u);
  DONE();
}

void lpuart_pclk1(void)
{
  SystemCoreClockUpdate();
  lpuart_send(SystemCoreClock, 115200u);
  DONE();
}

static void start_usart2(void)
{
  LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_USART2);
  SystemCoreClockUpdate();
  init(USART2, SystemCoreClock);
}

static void dma_channel(uint32_t channel, uint32_t direction, uint32_t memory, uint32_t items)
{
  LL_AHB1_GRP1_EnableClock(LL_AHB1_GRP1_PERIPH_DMA1);
  LL_DMA_SetPeriphRequest(DMA1, channel, LL_DMA_REQUEST_2);
  LL_DMA_ConfigTransfer(
      DMA1, channel,
      direction
          | LL_DMA_MODE_NORMAL
          | LL_DMA_PERIPH_NOINCREMENT
          | LL_DMA_MEMORY_INCREMENT
          | LL_DMA_PDATAALIGN_BYTE
          | LL_DMA_MDATAALIGN_BYTE
          | LL_DMA_PRIORITY_LOW);
  uint32_t const peripheral = LL_USART_DMA_GetRegAddr(
      USART2, direction == LL_DMA_DIRECTION_PERIPH_TO_MEMORY ? LL_USART_DMA_REG_DATA_RECEIVE
                                                             : LL_USART_DMA_REG_DATA_TRANSMIT);
  LL_DMA_ConfigAddresses(DMA1, channel, peripheral, memory, direction);
  if(direction == LL_DMA_DIRECTION_MEMORY_TO_PERIPH)
  {
    LL_DMA_ConfigAddresses(DMA1, channel, memory, peripheral, direction);
  }
  LL_DMA_SetDataLength(DMA1, channel, items);
  LL_DMA_EnableChannel(DMA1, channel);
}

void dma_tx(void)
{
  start_usart2();
  LL_USART_ClearFlag_TC(USART2);
  dma_channel(LL_DMA_CHANNEL_7, LL_DMA_DIRECTION_MEMORY_TO_PERIPH, (uint32_t)hello, 7u);
  LL_USART_EnableDMAReq_TX(USART2);
  while(LL_DMA_IsActiveFlag_TC7(DMA1) == 0u)
  {
  }
  while(!LL_USART_IsActiveFlag_TC(USART2))
  {
  }
  LOG(DMA1_CSELR->CSELR);
  LOG(LL_DMA_GetDataLength(DMA1, LL_DMA_CHANNEL_7));
  DONE();
}

void dma_rx(void)
{
  start_usart2();
  dma_channel(LL_DMA_CHANNEL_6, LL_DMA_DIRECTION_PERIPH_TO_MEMORY, (uint32_t)received, 4u);
  LL_USART_EnableDMAReq_RX(USART2);
  while(LL_DMA_IsActiveFlag_TC6(DMA1) == 0u)
  {
  }
  LOG(received[0]);
  LOG(received[3]);
  LOG(LL_DMA_GetDataLength(DMA1, LL_DMA_CHANNEL_6));
  LOG(USART2->ISR & (USART_ISR_RXNE | USART_ISR_ORE));
  DONE();
}

void idle_overrun(void)
{
  start_usart2();
  while(!LL_USART_IsActiveFlag_IDLE(USART2))
  {
  }
  LOG(USART2->ISR & (USART_ISR_RXNE | USART_ISR_ORE | USART_ISR_IDLE));
  LL_USART_ClearFlag_ORE(USART2);
  LL_USART_ClearFlag_IDLE(USART2);
  LOG(USART2->ISR & (USART_ISR_RXNE | USART_ISR_ORE | USART_ISR_IDLE));
  LOG(LL_USART_ReceiveData8(USART2));
  LOG(USART2->ISR & USART_ISR_RXNE);
  DONE();
}
