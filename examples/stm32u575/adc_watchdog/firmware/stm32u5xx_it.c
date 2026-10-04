#include "main.h"

void SysTick_Handler(void);
void ADC1_IRQHandler(void);
void GPDMA1_Channel0_IRQHandler(void);

void SysTick_Handler(void)
{
  HAL_IncTick();
}

void ADC1_IRQHandler(void)
{
  HAL_ADC_IRQHandler(&hadc1);
}

void GPDMA1_Channel0_IRQHandler(void)
{
  HAL_DMA_IRQHandler(&handle_GPDMA1_Channel0);
}
