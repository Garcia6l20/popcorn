#include "main.h"

void SysTick_Handler(void);
void RNG_IRQHandler(void);

void SysTick_Handler(void)
{
  HAL_IncTick();
}

void RNG_IRQHandler(void)
{
  HAL_RNG_IRQHandler(&hrng);
}
