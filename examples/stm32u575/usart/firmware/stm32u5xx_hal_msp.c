#include "main.h"

void HAL_UART_MspInit(UART_HandleTypeDef* huart)
{
  if(huart->Instance != USART1)
  {
    return;
  }
  __HAL_RCC_USART1_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  GPIO_InitTypeDef pins = {
      .Pin       = GPIO_PIN_9 | GPIO_PIN_10,
      .Mode      = GPIO_MODE_AF_PP,
      .Pull      = GPIO_NOPULL,
      .Speed     = GPIO_SPEED_FREQ_LOW,
      .Alternate = GPIO_AF7_USART1,
  };
  HAL_GPIO_Init(GPIOA, &pins);
  HAL_NVIC_SetPriority(USART1_IRQn, 5, 0);
  HAL_NVIC_EnableIRQ(USART1_IRQn);
}
