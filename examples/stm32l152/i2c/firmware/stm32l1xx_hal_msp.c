#include "main.h"

void HAL_I2C_MspInit(I2C_HandleTypeDef* hi2c)
{
  if(hi2c->Instance != I2C1)
  {
    return;
  }
  __HAL_RCC_GPIOB_CLK_ENABLE();
  GPIO_InitTypeDef pins = {
      .Pin       = GPIO_PIN_8 | GPIO_PIN_9,
      .Mode      = GPIO_MODE_AF_OD,
      .Pull      = GPIO_PULLUP,
      .Speed     = GPIO_SPEED_FREQ_HIGH,
      .Alternate = GPIO_AF4_I2C1,
  };
  HAL_GPIO_Init(GPIOB, &pins);
  __HAL_RCC_I2C1_CLK_ENABLE();
}
