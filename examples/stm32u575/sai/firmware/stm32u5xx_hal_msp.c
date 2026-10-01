#include "main.h"

void HAL_SAI_MspInit(SAI_HandleTypeDef* hsai)
{
  if(hsai->Instance != SAI1_Block_A)
  {
    return;
  }
  __HAL_RCC_SAI1_CLK_ENABLE();
  __HAL_RCC_GPIOE_CLK_ENABLE();
  GPIO_InitTypeDef pins = {
      .Pin       = GPIO_PIN_4 | GPIO_PIN_5 | GPIO_PIN_6,
      .Mode      = GPIO_MODE_AF_PP,
      .Pull      = GPIO_NOPULL,
      .Speed     = GPIO_SPEED_FREQ_HIGH,
      .Alternate = GPIO_AF13_SAI1,
  };
  HAL_GPIO_Init(GPIOE, &pins);
}
