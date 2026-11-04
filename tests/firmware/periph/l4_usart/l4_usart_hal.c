#include "periph_test.h"
#include "stm32l4xx_hal.h"

static char const hello[] = "hello\r\n";

static UART_HandleTypeDef uart;
static DMA_HandleTypeDef  tx_dma;
static uint8_t            received[4];
static volatile uint32_t  tx_done;
static volatile uint32_t  rx_done;

void idle(void);
void USART2_IRQHandler(void);
void DMA1_Channel7_IRQHandler(void);
void SysTick_Handler(void);
void HAL_UART_TxCpltCallback(UART_HandleTypeDef* handle);
void HAL_UART_RxCpltCallback(UART_HandleTypeDef* handle);

void idle(void)
{
  for(;;)
  {
    __WFI();
  }
}

void SysTick_Handler(void)
{
  HAL_IncTick();
}

void USART2_IRQHandler(void)
{
  HAL_UART_IRQHandler(&uart);
}

void DMA1_Channel7_IRQHandler(void)
{
  HAL_DMA_IRQHandler(&tx_dma);
}

void HAL_UART_TxCpltCallback(UART_HandleTypeDef* handle)
{
  if(handle == &uart)
  {
    tx_done = 1u;
  }
}

void HAL_UART_RxCpltCallback(UART_HandleTypeDef* handle)
{
  if(handle == &uart)
  {
    rx_done = 1u;
  }
}

int main(void)
{
  HAL_Init();
  __HAL_RCC_USART2_CLK_ENABLE();
  __HAL_RCC_DMA1_CLK_ENABLE();
  uart.Instance          = USART2;
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
  tx_dma.Instance                 = DMA1_Channel7;
  tx_dma.Init.Request             = DMA_REQUEST_2;
  tx_dma.Init.Direction           = DMA_MEMORY_TO_PERIPH;
  tx_dma.Init.PeriphInc           = DMA_PINC_DISABLE;
  tx_dma.Init.MemInc              = DMA_MINC_ENABLE;
  tx_dma.Init.PeriphDataAlignment = DMA_PDATAALIGN_BYTE;
  tx_dma.Init.MemDataAlignment    = DMA_MDATAALIGN_BYTE;
  tx_dma.Init.Mode                = DMA_NORMAL;
  tx_dma.Init.Priority            = DMA_PRIORITY_LOW;
  if(HAL_DMA_Init(&tx_dma) != HAL_OK)
  {
    FAIL(2);
  }
  __HAL_LINKDMA(&uart, hdmatx, tx_dma);
  NVIC_EnableIRQ(DMA1_Channel7_IRQn);
  NVIC_EnableIRQ(USART2_IRQn);
  if(HAL_UART_Transmit_DMA(&uart, (uint8_t const*)hello, 7u) != HAL_OK)
  {
    FAIL(3);
  }
  while(tx_done == 0u)
  {
    __WFI();
  }
  LOG(DMA1_CSELR->CSELR);
  if(HAL_UART_Receive_IT(&uart, received, 4u) != HAL_OK)
  {
    FAIL(4);
  }
  while(rx_done == 0u)
  {
    __WFI();
  }
  LOG(received[0]);
  LOG(received[3]);
  DONE();
}
