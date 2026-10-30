#include "periph_test.h"
#include "stm32l1xx.h"
#include "stm32l1xx_hal.h"
#include "stm32l1xx_ll_bus.h"
#include "stm32l1xx_ll_dma.h"
#include "stm32l1xx_ll_usart.h"

static char const hello[] = "hello\r\n";

uint8_t                   received[16];
static volatile uint32_t  received_count;
static volatile uint32_t  tx_next;
volatile uint32_t         release;
static volatile uint32_t  hal_mode;
static volatile uint32_t  hal_received;
static UART_HandleTypeDef uart;

void idle(void);
void tx_irq(void);
void rx_irq(void);
void over8_poll(void);
void tc_clear(void);
void nine_bits(void);
void uart4_poll(void);
void dma_tx(void);
void dma_rx(void);
void idle_clear(void);
void late_idle(void);
void overrun(void);
void hal_tx_rx(void);
void USART1_IRQHandler(void);
void SysTick_Handler(void);
void HAL_UART_RxCpltCallback(UART_HandleTypeDef* handle);

void idle(void)
{
  for(;;)
  {
    __WFI();
  }
}

static void init(USART_TypeDef* usart, uint32_t oversampling, uint32_t baud)
{
  LL_USART_SetTransferDirection(usart, LL_USART_DIRECTION_TX_RX);
  LL_USART_ConfigCharacter(usart, LL_USART_DATAWIDTH_8B, LL_USART_PARITY_NONE, LL_USART_STOPBITS_1);
  LL_USART_SetOverSampling(usart, oversampling);
  SystemCoreClockUpdate();
  LL_USART_SetBaudRate(usart, SystemCoreClock, oversampling, baud);
  LL_USART_ConfigAsyncMode(usart);
  LL_USART_Enable(usart);
}

static void start_usart1(void)
{
  LL_APB2_GRP1_EnableClock(LL_APB2_GRP1_PERIPH_USART1);
  init(USART1, LL_USART_OVERSAMPLING_16, 115200u);
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

void SysTick_Handler(void)
{
  HAL_IncTick();
}

void HAL_UART_RxCpltCallback(UART_HandleTypeDef* handle)
{
  if(handle == &uart)
  {
    hal_received = 1u;
  }
}

void USART1_IRQHandler(void)
{
  if(hal_mode != 0u)
  {
    HAL_UART_IRQHandler(&uart);
    return;
  }
  if(LL_USART_IsActiveFlag_RXNE(USART1))
  {
    uint8_t const data = LL_USART_ReceiveData8(USART1);
    if(received_count < sizeof received)
    {
      received[received_count++] = data;
    }
  }
  if(LL_USART_IsEnabledIT_TXE(USART1) && LL_USART_IsActiveFlag_TXE(USART1))
  {
    if(hello[tx_next] != 0)
    {
      LL_USART_TransmitData8(USART1, (uint8_t)hello[tx_next++]);
    }
    else
    {
      LL_USART_DisableIT_TXE(USART1);
    }
  }
}

int main(void)
{
  LOG(USART1->SR);
  start_usart1();
  send_poll(USART1);
  LOG(LL_USART_IsActiveFlag_TC(USART1));
  DONE();
}

void tx_irq(void)
{
  start_usart1();
  NVIC_EnableIRQ(USART1_IRQn);
  LL_USART_EnableIT_TXE(USART1);
  while(LL_USART_IsEnabledIT_TXE(USART1))
  {
    __WFI();
  }
  while(!LL_USART_IsActiveFlag_TC(USART1))
  {
  }
  LOG(tx_next);
  DONE();
}

void rx_irq(void)
{
  start_usart1();
  NVIC_EnableIRQ(USART1_IRQn);
  LL_USART_EnableIT_RXNE(USART1);
  while(received_count < 4u)
  {
    __WFI();
  }
  LOG(received_count);
  LOG(received[0]);
  LOG(received[3]);
  LOG(USART1->SR & (USART_SR_RXNE | USART_SR_ORE));
  DONE();
}

void over8_poll(void)
{
  LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_USART2);
  init(USART2, LL_USART_OVERSAMPLING_8, 115200u);
  LOG(USART2->BRR);
  send_poll(USART2);
  DONE();
}

void tc_clear(void)
{
  start_usart1();
  LL_USART_TransmitData8(USART1, 0x55u);
  while(!LL_USART_IsActiveFlag_TC(USART1))
  {
  }
  LOG(USART1->SR & (USART_SR_TXE | USART_SR_TC));
  LL_USART_ClearFlag_TC(USART1);
  LOG(USART1->SR & (USART_SR_TXE | USART_SR_TC));
  USART1->SR = 0u;
  LOG(USART1->SR & USART_SR_TXE);
  DONE();
}

void nine_bits(void)
{
  start_usart1();
  LL_USART_Disable(USART1);
  LL_USART_ConfigCharacter(
      USART1, LL_USART_DATAWIDTH_9B, LL_USART_PARITY_NONE, LL_USART_STOPBITS_2);
  LL_USART_Enable(USART1);
  LL_USART_TransmitData9(USART1, 0x1A5u);
  while(!LL_USART_IsActiveFlag_TXE(USART1))
  {
  }
  LL_USART_TransmitData9(USART1, 0x0FFu);
  while(!LL_USART_IsActiveFlag_TC(USART1))
  {
  }
  DONE();
}

void uart4_poll(void)
{
#if defined(UART4)
  LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_UART4);
  init(UART4, LL_USART_OVERSAMPLING_16, 9600u);
  send_poll(UART4);
  LOG(LL_USART_IsActiveFlag_TC(UART4));
#endif
  DONE();
}

static void dma_channel(uint32_t channel, uint32_t direction, uint32_t memory, uint32_t items)
{
  LL_AHB1_GRP1_EnableClock(LL_AHB1_GRP1_PERIPH_DMA1);
  LL_DMA_ConfigTransfer(
      DMA1, channel,
      direction
          | LL_DMA_MODE_NORMAL
          | LL_DMA_PERIPH_NOINCREMENT
          | LL_DMA_MEMORY_INCREMENT
          | LL_DMA_PDATAALIGN_BYTE
          | LL_DMA_MDATAALIGN_BYTE
          | LL_DMA_PRIORITY_LOW);
  uint32_t const peripheral = LL_USART_DMA_GetRegAddr(USART1);
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

void dma_tx(void)
{
  start_usart1();
  LL_USART_ClearFlag_TC(USART1);
  dma_channel(LL_DMA_CHANNEL_4, LL_DMA_DIRECTION_MEMORY_TO_PERIPH, (uint32_t)hello, 7u);
  LL_USART_EnableDMAReq_TX(USART1);
  while(LL_DMA_IsActiveFlag_TC4(DMA1) == 0u)
  {
  }
  LOG(LL_USART_IsActiveFlag_TC(USART1));
  while(!LL_USART_IsActiveFlag_TC(USART1))
  {
  }
  LOG(LL_DMA_GetDataLength(DMA1, LL_DMA_CHANNEL_4));
  LOG(LL_USART_IsActiveFlag_TC(USART1));
  DONE();
}

void dma_rx(void)
{
  start_usart1();
  dma_channel(LL_DMA_CHANNEL_5, LL_DMA_DIRECTION_PERIPH_TO_MEMORY, (uint32_t)received, 4u);
  LL_USART_EnableDMAReq_RX(USART1);
  while(LL_DMA_IsActiveFlag_TC5(DMA1) == 0u)
  {
  }
  LOG(received[0]);
  LOG(received[3]);
  LOG(LL_DMA_GetDataLength(DMA1, LL_DMA_CHANNEL_5));
  LOG(USART1->SR & (USART_SR_RXNE | USART_SR_ORE));
  DONE();
}

void idle_clear(void)
{
  start_usart1();
  while(!LL_USART_IsActiveFlag_IDLE(USART1))
  {
  }
  LOG(USART1->SR & (USART_SR_RXNE | USART_SR_IDLE));
  LOG(LL_USART_ReceiveData8(USART1));
  LOG(USART1->SR & (USART_SR_RXNE | USART_SR_IDLE));
  DONE();
}

void late_idle(void)
{
  start_usart1();
  while(!LL_USART_IsActiveFlag_RXNE(USART1))
  {
  }
  LOG(USART1->SR & USART_SR_IDLE);
  while(release == 0u)
  {
  }
  LOG(LL_USART_ReceiveData8(USART1));
  LOG(USART1->SR & USART_SR_IDLE);
  LL_USART_ClearFlag_IDLE(USART1);
  LOG(USART1->SR & USART_SR_IDLE);
  DONE();
}

void overrun(void)
{
  start_usart1();
  while(!LL_USART_IsActiveFlag_ORE(USART1))
  {
  }
  LOG(USART1->SR & (USART_SR_RXNE | USART_SR_ORE));
  LOG(LL_USART_ReceiveData8(USART1));
  LOG(USART1->SR & (USART_SR_RXNE | USART_SR_ORE));
  DONE();
}

void hal_tx_rx(void)
{
  HAL_Init();
  __HAL_RCC_USART1_CLK_ENABLE();
  uart.Instance          = USART1;
  uart.Init.BaudRate     = 115200u;
  uart.Init.WordLength   = UART_WORDLENGTH_8B;
  uart.Init.StopBits     = UART_STOPBITS_1;
  uart.Init.Parity       = UART_PARITY_NONE;
  uart.Init.Mode         = UART_MODE_TX_RX;
  uart.Init.HwFlowCtl    = UART_HWCONTROL_NONE;
  uart.Init.OverSampling = UART_OVERSAMPLING_16;
  if(HAL_UART_Init(&uart) != HAL_OK)
  {
    FAIL(1);
  }
  if(HAL_UART_Transmit(&uart, (uint8_t const*)hello, 7u, 100u) != HAL_OK)
  {
    FAIL(2);
  }
  hal_mode = 1u;
  NVIC_EnableIRQ(USART1_IRQn);
  if(HAL_UART_Receive_IT(&uart, received, 4u) != HAL_OK)
  {
    FAIL(3);
  }
  LOG(0x5a5au);
  while(hal_received == 0u)
  {
    __WFI();
  }
  LOG(received[0]);
  LOG(received[3]);
  LOG(uart.ErrorCode);
  DONE();
}
