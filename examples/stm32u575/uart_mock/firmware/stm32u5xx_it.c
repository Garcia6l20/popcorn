#include "main.h"

void SysTick_Handler(void);
void USART2_IRQHandler(void);

void SysTick_Handler(void)
{
  HAL_IncTick();
}

void USART2_IRQHandler(void)
{
  HAL_UART_IRQHandler(&huart2);
}
