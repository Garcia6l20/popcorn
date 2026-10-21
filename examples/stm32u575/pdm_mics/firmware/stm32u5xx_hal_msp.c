#include "main.h"

void HAL_MspInit(void)
{
  __HAL_RCC_PWR_CLK_ENABLE();
}

void HAL_MDF_MspInit(MDF_HandleTypeDef* hmdf)
{
  if(hmdf->Instance != MDF1_Filter0)
  {
    return;
  }
  __HAL_RCC_MDF1_CLK_ENABLE();
  __HAL_RCC_GPIOD_CLK_ENABLE();
  __HAL_RCC_GPIOE_CLK_ENABLE();
  GPIO_InitTypeDef clock = {
      .Pin       = GPIO_PIN_9,
      .Mode      = GPIO_MODE_AF_PP,
      .Pull      = GPIO_NOPULL,
      .Speed     = GPIO_SPEED_FREQ_HIGH,
      .Alternate = GPIO_AF6_MDF1,
  };
  HAL_GPIO_Init(GPIOE, &clock);
  GPIO_InitTypeDef data = {
      .Pin       = GPIO_PIN_3 | GPIO_PIN_6,
      .Mode      = GPIO_MODE_AF_PP,
      .Pull      = GPIO_NOPULL,
      .Speed     = GPIO_SPEED_FREQ_HIGH,
      .Alternate = GPIO_AF6_MDF1,
  };
  HAL_GPIO_Init(GPIOD, &data);
  HAL_NVIC_SetPriority(MDF1_FLT0_IRQn, 6, 0);
  HAL_NVIC_EnableIRQ(MDF1_FLT0_IRQn);
}
