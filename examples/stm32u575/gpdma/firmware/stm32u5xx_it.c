#include "main.h"

void SysTick_Handler(void);
void GPDMA1_Channel0_IRQHandler(void);
void USART1_IRQHandler(void);

void SysTick_Handler(void)
{
  HAL_IncTick();
}

void GPDMA1_Channel0_IRQHandler(void)
{
  HAL_DMA_IRQHandler(&handle_GPDMA1_Channel0);
}

void USART1_IRQHandler(void)
{
  HAL_UART_IRQHandler(&huart1);
}
