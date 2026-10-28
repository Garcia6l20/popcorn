#include "main.h"

void SysTick_Handler(void);
void UART9_IRQHandler(void);

void SysTick_Handler(void)
{
  HAL_IncTick();
}

void UART9_IRQHandler(void)
{
  HAL_UART_IRQHandler(&huart9);
}
