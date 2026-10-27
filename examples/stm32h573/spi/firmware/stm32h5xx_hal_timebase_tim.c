#include "main.h"

TIM_HandleTypeDef htim6;

HAL_StatusTypeDef HAL_InitTick(uint32_t TickPriority)
{
  if(TickPriority >= (1UL << __NVIC_PRIO_BITS))
  {
    return HAL_ERROR;
  }
  __HAL_RCC_TIM6_CLK_ENABLE();
  htim6.Instance               = TIM6;
  htim6.Init.Prescaler         = HAL_RCC_GetPCLK1Freq() / 1000000U - 1U;
  htim6.Init.CounterMode       = TIM_COUNTERMODE_UP;
  htim6.Init.Period            = 1000U * (uint32_t)uwTickFreq - 1U;
  htim6.Init.ClockDivision     = TIM_CLOCKDIVISION_DIV1;
  htim6.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
  if(HAL_TIM_Base_Init(&htim6) != HAL_OK)
  {
    return HAL_ERROR;
  }
  HAL_NVIC_SetPriority(TIM6_IRQn, TickPriority, 0U);
  HAL_NVIC_EnableIRQ(TIM6_IRQn);
  uwTickPrio = TickPriority;
  return HAL_TIM_Base_Start_IT(&htim6);
}

void HAL_SuspendTick(void)
{
  __HAL_TIM_DISABLE_IT(&htim6, TIM_IT_UPDATE);
}

void HAL_ResumeTick(void)
{
  __HAL_TIM_ENABLE_IT(&htim6, TIM_IT_UPDATE);
}
