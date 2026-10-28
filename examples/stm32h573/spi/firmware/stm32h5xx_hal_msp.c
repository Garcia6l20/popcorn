#include "main.h"

void HAL_SPI_MspInit(SPI_HandleTypeDef* hspi)
{
  if(hspi->Instance != SPI4)
  {
    return;
  }
  __HAL_RCC_SPI4_CLK_ENABLE();
  __HAL_RCC_GPIOE_CLK_ENABLE();
  GPIO_InitTypeDef pins = {
      .Pin       = GPIO_PIN_2 | GPIO_PIN_4 | GPIO_PIN_5 | GPIO_PIN_6,
      .Mode      = GPIO_MODE_AF_PP,
      .Pull      = GPIO_NOPULL,
      .Speed     = GPIO_SPEED_FREQ_HIGH,
      .Alternate = GPIO_AF5_SPI4,
  };
  HAL_GPIO_Init(GPIOE, &pins);
}
