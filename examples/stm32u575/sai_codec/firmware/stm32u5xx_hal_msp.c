#include "main.h"

void HAL_SAI_MspInit(SAI_HandleTypeDef* hsai)
{
  __HAL_RCC_SAI1_CLK_ENABLE();
  __HAL_RCC_GPIOE_CLK_ENABLE();
  GPIO_InitTypeDef pins = {
      .Pin   = hsai->Instance == SAI1_Block_A ? GPIO_PIN_4 | GPIO_PIN_5 | GPIO_PIN_6 : GPIO_PIN_3,
      .Mode  = GPIO_MODE_AF_PP,
      .Pull  = GPIO_NOPULL,
      .Speed = GPIO_SPEED_FREQ_HIGH,
      .Alternate = GPIO_AF13_SAI1,
  };
  HAL_GPIO_Init(GPIOE, &pins);
}

void HAL_I2C_MspInit(I2C_HandleTypeDef* hi2c)
{
  if(hi2c->Instance != I2C1)
  {
    return;
  }
  __HAL_RCC_GPIOB_CLK_ENABLE();
  GPIO_InitTypeDef pins = {
      .Pin       = GPIO_PIN_6 | GPIO_PIN_7,
      .Mode      = GPIO_MODE_AF_OD,
      .Pull      = GPIO_NOPULL,
      .Speed     = GPIO_SPEED_FREQ_LOW,
      .Alternate = GPIO_AF4_I2C1,
  };
  HAL_GPIO_Init(GPIOB, &pins);
  __HAL_RCC_I2C1_CLK_ENABLE();
}
