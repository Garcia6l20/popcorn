#include "main.h"

void SysTick_Handler(void);
void GPDMA1_Channel0_IRQHandler(void);
void MDF1_FLT0_IRQHandler(void);

void SysTick_Handler(void)
{
  HAL_IncTick();
}

void GPDMA1_Channel0_IRQHandler(void)
{
  HAL_DMA_IRQHandler(&handle_GPDMA1_Channel0);
}

void MDF1_FLT0_IRQHandler(void)
{
  HAL_MDF_IRQHandler(&mdf_filter0);
}
