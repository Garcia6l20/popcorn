#include "main.h"

void HAL_UART_MspInit(UART_HandleTypeDef* huart)
{
  if(huart->Instance != UART9)
  {
    return;
  }
  __HAL_RCC_UART9_CLK_ENABLE();
  __HAL_RCC_GPIOG_CLK_ENABLE();
  GPIO_InitTypeDef pins = {
      .Pin       = GPIO_PIN_1,
      .Mode      = GPIO_MODE_AF_PP,
      .Pull      = GPIO_NOPULL,
      .Speed     = GPIO_SPEED_FREQ_LOW,
      .Alternate = GPIO_AF11_UART9,
  };
  HAL_GPIO_Init(GPIOG, &pins);
  HAL_NVIC_SetPriority(UART9_IRQn, 5, 0);
  HAL_NVIC_EnableIRQ(UART9_IRQn);
}
