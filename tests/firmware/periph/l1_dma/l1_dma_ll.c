#include "periph_test.h"
#include "stm32l1xx.h"
#include "stm32l1xx_ll_bus.h"
#include "stm32l1xx_ll_dma.h"

#define UNMAPPED 0x60000000u

uint32_t source[16];
uint32_t destination[16];

static volatile uint32_t halves;
static volatile uint32_t completions;
static volatile uint32_t errors;

void idle(void);
void DMA1_Channel2_IRQHandler(void);
void DMA1_Channel3_IRQHandler(void);

void idle(void)
{
  for(;;)
  {
    __WFI();
  }
}

void DMA1_Channel2_IRQHandler(void)
{
  if(LL_DMA_IsActiveFlag_TC2(DMA1) != 0u)
  {
    LL_DMA_ClearFlag_TC2(DMA1);
    LOG(0x7c00u | LL_DMA_GetDataLength(DMA1, LL_DMA_CHANNEL_2));
    ++completions;
  }
  else if(LL_DMA_IsActiveFlag_HT2(DMA1) != 0u)
  {
    LL_DMA_ClearFlag_HT2(DMA1);
    ++halves;
  }
}

void DMA1_Channel3_IRQHandler(void)
{
  if(LL_DMA_IsActiveFlag_TE3(DMA1) != 0u)
  {
    LL_DMA_DisableIT_TE(DMA1, LL_DMA_CHANNEL_3);
    ++errors;
  }
}

static void fill(void)
{
  for(uint32_t index = 0; index < 16u; ++index)
  {
    source[index]      = 0x11223344u * (index + 1u) ^ 0xa5a5a5a5u;
    destination[index] = 0xffffffffu;
  }
}

static void copy(uint32_t channel, uint32_t sizes, uint32_t from, uint32_t to, uint32_t items)
{
  LL_DMA_ConfigTransfer(
      DMA1, channel,
      LL_DMA_DIRECTION_MEMORY_TO_MEMORY
          | LL_DMA_MODE_NORMAL
          | LL_DMA_PERIPH_INCREMENT
          | LL_DMA_MEMORY_INCREMENT
          | sizes
          | LL_DMA_PRIORITY_LOW);
  LL_DMA_ConfigAddresses(DMA1, channel, from, to, LL_DMA_DIRECTION_MEMORY_TO_MEMORY);
  LL_DMA_SetDataLength(DMA1, channel, items);
  LL_DMA_EnableChannel(DMA1, channel);
}

static void copy_and_log(uint32_t sizes, uint32_t items)
{
  fill();
  copy(LL_DMA_CHANNEL_1, sizes, (uint32_t)source, (uint32_t)destination, items);
  while(LL_DMA_IsActiveFlag_TC1(DMA1) == 0u)
  {
  }
  LOG(DMA1->ISR & 0xfu);
  LOG(LL_DMA_GetDataLength(DMA1, LL_DMA_CHANNEL_1));
  LOG(LL_DMA_IsEnabledChannel(DMA1, LL_DMA_CHANNEL_1));
  LOG(destination[0]);
  LOG(destination[1]);
  LOG(destination[items - 1u]);
  LL_DMA_DisableChannel(DMA1, LL_DMA_CHANNEL_1);
  LL_DMA_ClearFlag_GI1(DMA1);
}

int main(void)
{
  LL_AHB1_GRP1_EnableClock(LL_AHB1_GRP1_PERIPH_DMA1);

  copy_and_log(LL_DMA_PDATAALIGN_WORD | LL_DMA_MDATAALIGN_WORD, 16u);
  copy_and_log(LL_DMA_PDATAALIGN_BYTE | LL_DMA_MDATAALIGN_HALFWORD, 4u);
  copy_and_log(LL_DMA_PDATAALIGN_WORD | LL_DMA_MDATAALIGN_BYTE, 4u);
  copy_and_log(LL_DMA_PDATAALIGN_HALFWORD | LL_DMA_MDATAALIGN_WORD, 4u);

  fill();
  copy(
      LL_DMA_CHANNEL_1, LL_DMA_PDATAALIGN_WORD | LL_DMA_MDATAALIGN_WORD, (uint32_t)source,
      (uint32_t)destination, 8u);
  while(LL_DMA_IsActiveFlag_TC1(DMA1) == 0u)
  {
  }
  LL_DMA_ClearFlag_TC1(DMA1);
  LOG(DMA1->ISR & 0xfu);
  LL_DMA_ClearFlag_HT1(DMA1);
  LOG(DMA1->ISR & 0xfu);
  LL_DMA_DisableChannel(DMA1, LL_DMA_CHANNEL_1);

  fill();
  LL_DMA_EnableIT_HT(DMA1, LL_DMA_CHANNEL_2);
  LL_DMA_EnableIT_TC(DMA1, LL_DMA_CHANNEL_2);
  NVIC_EnableIRQ(DMA1_Channel2_IRQn);
  copy(
      LL_DMA_CHANNEL_2, LL_DMA_PDATAALIGN_WORD | LL_DMA_MDATAALIGN_WORD, (uint32_t)source,
      (uint32_t)destination, 16u);
  while(completions == 0u)
  {
    __WFI();
  }
  LOG(halves);
  LOG(destination[15] == source[15] ? 1u : 0u);
  LL_DMA_DisableChannel(DMA1, LL_DMA_CHANNEL_2);

  LL_DMA_EnableIT_TE(DMA1, LL_DMA_CHANNEL_3);
  NVIC_EnableIRQ(DMA1_Channel3_IRQn);
  copy(
      LL_DMA_CHANNEL_3, LL_DMA_PDATAALIGN_WORD | LL_DMA_MDATAALIGN_WORD, UNMAPPED,
      (uint32_t)destination, 4u);
  while(errors == 0u)
  {
    __WFI();
  }
  LOG(errors);
  LOG((DMA1->ISR >> 8) & 0xfu);
  LOG(LL_DMA_IsEnabledChannel(DMA1, LL_DMA_CHANNEL_3));
  LOG(LL_DMA_GetDataLength(DMA1, LL_DMA_CHANNEL_3));
  LL_DMA_ClearFlag_TE3(DMA1);
  LOG((DMA1->ISR >> 8) & 0xfu);
  DONE();
}
