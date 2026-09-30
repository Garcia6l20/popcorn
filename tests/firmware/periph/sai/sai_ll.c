#include "periph_test.h"
#include "stm32u5xx.h"

#define FRAMES       64u
#define DUPLEX_WORDS 64u

#define SAI_CR1_TX_MASTER_16 ((4u << SAI_xCR1_DS_Pos) | (4u << SAI_xCR1_MCKDIV_Pos))
#define SAI_CR1_RX_SYNC_16   (SAI_xCR1_MODE | (4u << SAI_xCR1_DS_Pos) | SAI_xCR1_SYNCEN_0)
#define SAI_FRCR_I2S_32 \
  ((31u << SAI_xFRCR_FRL_Pos) | (15u << SAI_xFRCR_FSALL_Pos) | SAI_xFRCR_FSDEF | SAI_xFRCR_FSOFF)
#define SAI_SLOTR_STEREO ((1u << SAI_xSLOTR_NBSLOT_Pos) | (3u << SAI_xSLOTR_SLOTEN_Pos))

uint32_t          received[DUPLEX_WORDS];
volatile uint32_t received_count;

static volatile uint32_t sent_count;
static volatile uint32_t underruns;
static volatile uint32_t status_seen;

void idle(void);
void tx_poll(void);
void duplex_irq(void);
void underrun(void);
void SAI1_IRQHandler(void);
int  main(void);

void idle(void)
{
  for(;;)
  {
    __WFI();
  }
}

static uint32_t ramp(uint32_t index)
{
  return 0x1000u + index;
}

static void clocks(void)
{
  RCC->CR |= RCC_CR_HSION;
  while((RCC->CR & RCC_CR_HSIRDY) == 0)
  {
  }
  RCC->CCIPR2 = (RCC->CCIPR2 & ~RCC_CCIPR2_SAI1SEL) | (4u << RCC_CCIPR2_SAI1SEL_Pos);
  RCC->APB2ENR |= RCC_APB2ENR_SAI1EN;
  (void)RCC->APB2ENR;
}

static void configure(SAI_Block_TypeDef* block, uint32_t cr1, uint32_t imr)
{
  block->CR1   = cr1;
  block->CR2   = SAI_xCR2_FFLUSH;
  block->FRCR  = SAI_FRCR_I2S_32;
  block->SLOTR = SAI_SLOTR_STEREO;
  block->CLRFR = SAI_xCLRFR_COVRUDR | SAI_xCLRFR_CWCKCFG;
  block->IMR   = imr;
}

static void disable(SAI_Block_TypeDef* block)
{
  block->CR1 &= ~SAI_xCR1_SAIEN;
  while((block->CR1 & SAI_xCR1_SAIEN) != 0)
  {
  }
}

void tx_poll(void)
{
  clocks();
  configure(SAI1_Block_A, SAI_CR1_TX_MASTER_16, 0);
  SAI1_Block_A->CR1 |= SAI_xCR1_SAIEN;
  for(uint32_t index = 0; index < 2u * FRAMES; ++index)
  {
    while((SAI1_Block_A->SR & SAI_xSR_FREQ) == 0)
    {
    }
    SAI1_Block_A->DR = ramp(index);
  }
  while((SAI1_Block_A->SR & SAI_xSR_FLVL) != 0)
  {
  }
  disable(SAI1_Block_A);
  LOG(SAI1_Block_A->SR & SAI_xSR_OVRUDR);
  DONE();
}

void SAI1_IRQHandler(void)
{
  uint32_t const tx = SAI1_Block_A->SR;
  if((tx & SAI_xSR_OVRUDR) != 0 && (SAI1_Block_A->IMR & SAI_xIMR_OVRUDRIE) != 0)
  {
    status_seen = tx;
    ++underruns;
    SAI1_Block_A->CLRFR = SAI_xCLRFR_COVRUDR;
  }
  while((SAI1_Block_A->IMR & SAI_xIMR_FREQIE) != 0 && (SAI1_Block_A->SR & SAI_xSR_FREQ) != 0)
  {
    SAI1_Block_A->DR = ramp(sent_count);
    if(++sent_count == DUPLEX_WORDS)
    {
      SAI1_Block_A->IMR &= ~SAI_xIMR_FREQIE;
    }
  }
  while((SAI1_Block_B->IMR & SAI_xIMR_FREQIE) != 0 && (SAI1_Block_B->SR & SAI_xSR_FREQ) != 0)
  {
    uint32_t const word = SAI1_Block_B->DR;
    if(received_count < DUPLEX_WORDS)
    {
      received[received_count++] = word;
    }
  }
}

void duplex_irq(void)
{
  clocks();
  configure(SAI1_Block_B, SAI_CR1_RX_SYNC_16, SAI_xIMR_FREQIE);
  configure(SAI1_Block_A, SAI_CR1_TX_MASTER_16, SAI_xIMR_FREQIE);
  NVIC_EnableIRQ(SAI1_IRQn);
  SAI1_Block_B->CR1 |= SAI_xCR1_SAIEN;
  SAI1_Block_A->CR1 |= SAI_xCR1_SAIEN;
  while(received_count < DUPLEX_WORDS)
  {
    __WFI();
  }
  disable(SAI1_Block_A);
  disable(SAI1_Block_B);
  NVIC_DisableIRQ(SAI1_IRQn);
  LOG(sent_count);
  LOG(received_count);
  DONE();
}

void underrun(void)
{
  clocks();
  configure(SAI1_Block_A, SAI_CR1_TX_MASTER_16, SAI_xIMR_OVRUDRIE);
  NVIC_EnableIRQ(SAI1_IRQn);
  SAI1_Block_A->DR = ramp(0);
  SAI1_Block_A->DR = ramp(1);
  SAI1_Block_A->DR = ramp(2);
  SAI1_Block_A->CR1 |= SAI_xCR1_SAIEN;
  while(underruns == 0)
  {
    __WFI();
  }
  LOG(status_seen & SAI_xSR_OVRUDR);
  LOG(SAI1_Block_A->SR & SAI_xSR_OVRUDR);
  SAI1_Block_A->DR = ramp(3);
  SAI1_Block_A->DR = ramp(4);
  while(underruns == 1)
  {
    __WFI();
  }
  disable(SAI1_Block_A);
  NVIC_DisableIRQ(SAI1_IRQn);
  LOG(underruns);
  DONE();
}

int main(void)
{
  idle();
}
