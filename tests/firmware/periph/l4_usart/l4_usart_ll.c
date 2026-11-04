#include "periph_test.h"
#include "stm32l4xx.h"
#include "stm32l4xx_ll_bus.h"
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
