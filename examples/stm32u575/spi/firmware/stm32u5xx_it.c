#include "main.h"

void TIM6_IRQHandler(void);

void TIM6_IRQHandler(void)
{
  HAL_TIM_IRQHandler(&htim6);
}
