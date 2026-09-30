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
