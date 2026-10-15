#include "main.h"

void HAL_RNG_MspInit(RNG_HandleTypeDef* handle)
{
  if(handle->Instance != RNG)
  {
    return;
  }
  __HAL_RCC_RNG_CONFIG(RCC_RNGCLKSOURCE_HSI48);
  __HAL_RCC_RNG_CLK_ENABLE();
  HAL_NVIC_SetPriority(RNG_IRQn, 5, 0);
  HAL_NVIC_EnableIRQ(RNG_IRQn);
}

void HAL_SPI_MspInit(SPI_HandleTypeDef* hspi)
{
  if(hspi->Instance != SPI1)
  {
    return;
  }
  __HAL_RCC_SPI1_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  GPIO_InitTypeDef pins = {
      .Pin       = GPIO_PIN_5 | GPIO_PIN_6 | GPIO_PIN_7,
      .Mode      = GPIO_MODE_AF_PP,
      .Pull      = GPIO_NOPULL,
      .Speed     = GPIO_SPEED_FREQ_HIGH,
      .Alternate = GPIO_AF5_SPI1,
  };
  HAL_GPIO_Init(GPIOA, &pins);
}
