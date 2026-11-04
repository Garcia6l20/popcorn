#include "periph_test.h"
#include "stm32l4xx.h"
#include "stm32l4xx_ll_bus.h"
#include "stm32l4xx_ll_dma.h"

uint32_t          source[16];
uint32_t          destination[16];
volatile uint32_t sink;

void idle(void);

void idle(void)
{
  for(;;)
  {
    __WFI();
  }
}

int main(void)
{
  LL_AHB1_GRP1_EnableClock(LL_AHB1_GRP1_PERIPH_DMA1);
  for(uint32_t index = 0; index < 16u; ++index)
  {
    source[index]      = 0x11223344u * (index + 1u) ^ 0xa5a5a5a5u;
    destination[index] = 0xffffffffu;
  }

  LL_DMA_ConfigTransfer(
      DMA1, LL_DMA_CHANNEL_1,
      LL_DMA_DIRECTION_MEMORY_TO_MEMORY
          | LL_DMA_MODE_NORMAL
          | LL_DMA_PERIPH_INCREMENT
          | LL_DMA_MEMORY_INCREMENT
          | LL_DMA_PDATAALIGN_WORD
          | LL_DMA_MDATAALIGN_WORD
          | LL_DMA_PRIORITY_LOW);
  LL_DMA_ConfigAddresses(
      DMA1, LL_DMA_CHANNEL_1, (uint32_t)source, (uint32_t)destination,
      LL_DMA_DIRECTION_MEMORY_TO_MEMORY);
  LL_DMA_SetDataLength(DMA1, LL_DMA_CHANNEL_1, 16u);
  LL_DMA_EnableChannel(DMA1, LL_DMA_CHANNEL_1);
  while(LL_DMA_IsActiveFlag_TC1(DMA1) == 0u)
  {
  }
  LOG(DMA1->ISR & 0xfu);
  LOG(LL_DMA_GetDataLength(DMA1, LL_DMA_CHANNEL_1));
  LOG(destination[0] == source[0]);
  LOG(destination[15] == source[15]);
  LL_DMA_ClearFlag_GI1(DMA1);

  LL_DMA_SetPeriphRequest(DMA1, LL_DMA_CHANNEL_7, LL_DMA_REQUEST_2);
  LL_DMA_ConfigTransfer(
      DMA1, LL_DMA_CHANNEL_7,
      LL_DMA_DIRECTION_MEMORY_TO_PERIPH
          | LL_DMA_MODE_NORMAL
          | LL_DMA_PERIPH_NOINCREMENT
          | LL_DMA_MEMORY_INCREMENT
          | LL_DMA_PDATAALIGN_WORD
          | LL_DMA_MDATAALIGN_WORD
          | LL_DMA_PRIORITY_LOW);
  LL_DMA_ConfigAddresses(
      DMA1, LL_DMA_CHANNEL_7, (uint32_t)source, (uint32_t)&sink, LL_DMA_DIRECTION_MEMORY_TO_PERIPH);
  LL_DMA_SetDataLength(DMA1, LL_DMA_CHANNEL_7, 4u);
  LL_DMA_EnableChannel(DMA1, LL_DMA_CHANNEL_7);
  LOG(DMA1_CSELR->CSELR);
  __asm__ volatile("bkpt #3" ::: "memory");
  while(LL_DMA_IsActiveFlag_TC7(DMA1) == 0u)
  {
  }
  LOG(LL_DMA_GetDataLength(DMA1, LL_DMA_CHANNEL_7));
  LOG(sink == source[3]);
  DONE();
}
