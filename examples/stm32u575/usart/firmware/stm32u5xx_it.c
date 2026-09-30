#include "main.h"

void SysTick_Handler(void);
void USART1_IRQHandler(void);

void SysTick_Handler(void)
{
  HAL_IncTick();
}

void USART1_IRQHandler(void)
{
  HAL_UART_IRQHandler(&huart1);
}
