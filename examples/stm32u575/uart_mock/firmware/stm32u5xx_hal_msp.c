#include "main.h"

void HAL_UART_MspInit(UART_HandleTypeDef* huart)
{
  if(huart->Instance != USART2)
  {
    return;
  }
  __HAL_RCC_USART2_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  GPIO_InitTypeDef pins = {
      .Pin       = GPIO_PIN_2 | GPIO_PIN_3,
      .Mode      = GPIO_MODE_AF_PP,
      .Pull      = GPIO_NOPULL,
      .Speed     = GPIO_SPEED_FREQ_LOW,
      .Alternate = GPIO_AF7_USART2,
  };
  HAL_GPIO_Init(GPIOA, &pins);
  HAL_NVIC_SetPriority(USART2_IRQn, 5, 0);
  HAL_NVIC_EnableIRQ(USART2_IRQn);
}
