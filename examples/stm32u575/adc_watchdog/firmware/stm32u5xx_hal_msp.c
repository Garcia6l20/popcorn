#include "main.h"

void HAL_ADC_MspInit(ADC_HandleTypeDef* hadc)
{
  if(hadc->Instance != ADC1)
  {
    return;
  }
  __HAL_RCC_PWR_CLK_ENABLE();
  HAL_PWREx_EnableVddA();
  __HAL_RCC_ADC12_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOC_CLK_ENABLE();
  GPIO_InitTypeDef pins = {
      .Pin  = GPIO_PIN_0 | GPIO_PIN_1,
      .Mode = GPIO_MODE_ANALOG,
      .Pull = GPIO_NOPULL,
  };
  HAL_GPIO_Init(GPIOA, &pins);
  HAL_GPIO_Init(GPIOC, &pins);
  HAL_NVIC_SetPriority(ADC1_IRQn, 5, 0);
  HAL_NVIC_EnableIRQ(ADC1_IRQn);
}

void HAL_TIM_Base_MspInit(TIM_HandleTypeDef* htim)
{
  if(htim->Instance == TIM15)
  {
    __HAL_RCC_TIM15_CLK_ENABLE();
  }
}
