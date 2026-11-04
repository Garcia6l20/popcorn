#include "periph_test.h"
#include "stm32l4xx.h"
#include "stm32l4xx_ll_bus.h"
#include "stm32l4xx_ll_dma.h"
#include "stm32l4xx_ll_rcc.h"

#define RING_WORDS  64u
#define RING_BLOCKS 4u
#define SAI_CR1_I2S_MASTER_TX \
  ((7u << SAI_xCR1_DS_Pos) | SAI_xCR1_CKSTR | (2u << SAI_xCR1_MCKDIV_Pos) | SAI_xCR1_DMAEN)
#define SAI_FRCR_I2S \
  ((63u << SAI_xFRCR_FRL_Pos) | (31u << SAI_xFRCR_FSALL_Pos) | SAI_xFRCR_FSDEF | SAI_xFRCR_FSOFF)
#define SAI_SLOTR_STEREO ((1u << SAI_xSLOTR_NBSLOT_Pos) | (3u << SAI_xSLOTR_SLOTEN_Pos))

uint32_t                 ring[RING_WORDS];
static volatile uint32_t ring_blocks;
static volatile uint32_t ring_halves;

void idle(void);
void tx_dma(void);
void DMA2_Channel1_IRQHandler(void);

void idle(void)
{
  for(;;)
  {
    __WFI();
  }
}

void DMA2_Channel1_IRQHandler(void)
{
  if(LL_DMA_IsActiveFlag_HT1(DMA2) != 0u)
  {
    LL_DMA_ClearFlag_HT1(DMA2);
    ++ring_halves;
  }
  if(LL_DMA_IsActiveFlag_TC1(DMA2) != 0u)
  {
    LL_DMA_ClearFlag_TC1(DMA2);
    ++ring_blocks;
  }
}

static void clocks(void)
{
  LL_RCC_HSI_Enable();
  while(LL_RCC_HSI_IsReady() == 0u)
  {
  }
  LL_RCC_PLLSAI1_ConfigDomain_SAI(
      LL_RCC_PLLSOURCE_HSI, LL_RCC_PLLM_DIV_2, 43u, LL_RCC_PLLSAI1P_DIV_7);
  LL_RCC_PLLSAI1_EnableDomain_SAI();
  LL_RCC_PLLSAI1_Enable();
  while(LL_RCC_PLLSAI1_IsReady() == 0u)
  {
  }
  LL_RCC_SetSAIClockSource(LL_RCC_SAI1_CLKSOURCE_PLLSAI1);
  LL_APB2_GRP1_EnableClock(LL_APB2_GRP1_PERIPH_SAI1);
  LL_AHB1_GRP1_EnableClock(LL_AHB1_GRP1_PERIPH_DMA2);
}

static void ring_channel(void)
{
  LL_DMA_SetPeriphRequest(DMA2, LL_DMA_CHANNEL_1, LL_DMA_REQUEST_1);
  LL_DMA_ConfigTransfer(
      DMA2, LL_DMA_CHANNEL_1,
      LL_DMA_DIRECTION_MEMORY_TO_PERIPH
          | LL_DMA_MODE_CIRCULAR
          | LL_DMA_PERIPH_NOINCREMENT
          | LL_DMA_MEMORY_INCREMENT
          | LL_DMA_PDATAALIGN_WORD
          | LL_DMA_MDATAALIGN_WORD
          | LL_DMA_PRIORITY_HIGH);
  LL_DMA_ConfigAddresses(
      DMA2, LL_DMA_CHANNEL_1, (uint32_t)ring, (uint32_t)&SAI1_Block_A->DR,
      LL_DMA_DIRECTION_MEMORY_TO_PERIPH);
  LL_DMA_SetDataLength(DMA2, LL_DMA_CHANNEL_1, RING_WORDS);
  LL_DMA_EnableIT_HT(DMA2, LL_DMA_CHANNEL_1);
  LL_DMA_EnableIT_TC(DMA2, LL_DMA_CHANNEL_1);
  NVIC_EnableIRQ(DMA2_Channel1_IRQn);
}

void tx_dma(void)
{
  for(uint32_t index = 0; index < RING_WORDS; ++index)
  {
    ring[index] = 0x5a000000u | (index << 8) | index;
  }
  clocks();
  SAI1_Block_A->CR1   = SAI_CR1_I2S_MASTER_TX;
  SAI1_Block_A->CR2   = SAI_xCR2_FFLUSH;
  SAI1_Block_A->FRCR  = SAI_FRCR_I2S;
  SAI1_Block_A->SLOTR = SAI_SLOTR_STEREO;
  SAI1_Block_A->CLRFR = SAI_xCLRFR_COVRUDR;
  ring_channel();
  LL_DMA_EnableChannel(DMA2, LL_DMA_CHANNEL_1);
  while((SAI1_Block_A->SR & SAI_xSR_FLVL) == 0u)
  {
  }
  LOG(SAI1_Block_A->SR & SAI_xSR_FLVL);
  SAI1_Block_A->CR1 |= SAI_xCR1_SAIEN;
  while(ring_blocks < RING_BLOCKS)
  {
    __WFI();
  }
  SAI1_Block_A->CR1 &= ~SAI_xCR1_SAIEN;
  while((SAI1_Block_A->CR1 & SAI_xCR1_SAIEN) != 0u)
  {
  }
  LL_DMA_DisableChannel(DMA2, LL_DMA_CHANNEL_1);
  LOG(ring_blocks);
  LOG(ring_halves);
  LOG(SAI1_Block_A->SR & SAI_xSR_OVRUDR);
  LOG(DMA2_CSELR->CSELR);
  DONE();
}

int main(void)
{
  DONE();
}
