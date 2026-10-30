#include "periph_test.h"
#include "stm32l1xx.h"
#include "stm32l1xx_ll_bus.h"
#include "stm32l1xx_ll_usart.h"

static char const hello[] = "hello\r\n";

uint8_t                  received[16];
static volatile uint32_t received_count;
static volatile uint32_t tx_next;

void idle(void);
void tx_irq(void);
void rx_irq(void);
void over8_poll(void);
void tc_clear(void);
void nine_bits(void);
void uart4_poll(void);
void USART1_IRQHandler(void);

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

void USART1_IRQHandler(void)
{
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
