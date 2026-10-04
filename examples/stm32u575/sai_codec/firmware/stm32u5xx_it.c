#include "main.h"

void SysTick_Handler(void);
void GPDMA1_Channel12_IRQHandler(void);
void GPDMA1_Channel13_IRQHandler(void);

void SysTick_Handler(void)
{
  HAL_IncTick();
}

void GPDMA1_Channel12_IRQHandler(void)
{
  HAL_DMA_IRQHandler(&handle_GPDMA1_Channel12);
}

void GPDMA1_Channel13_IRQHandler(void)
{
  HAL_DMA_IRQHandler(&handle_GPDMA1_Channel13);
}
