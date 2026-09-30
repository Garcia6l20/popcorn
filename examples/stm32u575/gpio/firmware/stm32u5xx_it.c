#include "main.h"

void SysTick_Handler(void);
void EXTI13_IRQHandler(void);

void SysTick_Handler(void)
{
  HAL_IncTick();
}

void EXTI13_IRQHandler(void)
{
  HAL_GPIO_EXTI_IRQHandler(BUTTON_PIN);
}
