#include "periph_test.h"
#include "stm32u5xx.h"
#include "stm32u5xx_ll_bus.h"
#include "stm32u5xx_ll_dma.h"
#include "stm32u5xx_ll_i2c.h"

#define FRAMES               64u
#define DUPLEX_WORDS         64u
#define RING_WORDS           64u
#define RING_BLOCKS          32u
#define SAMPLES              16u
#define SAI_CR1_TX_MASTER_32 ((7u << SAI_xCR1_DS_Pos) | (4u << SAI_xCR1_MCKDIV_Pos))
#define LINK_UPDATE          (LL_DMA_UPDATE_CBR1 | LL_DMA_UPDATE_CSAR | LL_DMA_UPDATE_CLLR)

#define SAI_CR1_TX_MASTER_16 ((4u << SAI_xCR1_DS_Pos) | (4u << SAI_xCR1_MCKDIV_Pos))
#define SAI_CR1_RX_SYNC_16   (SAI_xCR1_MODE | (4u << SAI_xCR1_DS_Pos) | SAI_xCR1_SYNCEN_0)
#define SAI_FRCR_I2S_32 \
  ((31u << SAI_xFRCR_FRL_Pos) | (15u << SAI_xFRCR_FSALL_Pos) | SAI_xFRCR_FSDEF | SAI_xFRCR_FSOFF)
#define SAI_SLOTR_STEREO ((1u << SAI_xSLOTR_NBSLOT_Pos) | (3u << SAI_xSLOTR_SLOTEN_Pos))

#define STEREO_FRAMES       512u
#define STEREO_EVENTS       5u
#define STEREO_HALVES       3u
#define STEREO_TX_CR1_SLAVE 0x00000282u
#define STEREO_RX_CR1_SYNC  0x00000683u
#define STEREO_TX_CR1_MASTER \
  ((4u << SAI_xCR1_DS_Pos) | SAI_xCR1_CKSTR | (4u << SAI_xCR1_MCKDIV_Pos))
#define STEREO_FRCR         0x00051F3Fu
#define STEREO_SLOTR        0x00030100u
#define STEREO_IMR          0x61u
#define STEREO_CCR          0x00421F00u
#define STEREO_TX_CTR1      0x40020009u
#define STEREO_TX_CBR1      0x41FF0004u
#define STEREO_TX_CTR3      0x000003FEu
#define STEREO_TX_CBR2      0x000007FEu
#define STEREO_RX_CTR1      0x40090002u
#define STEREO_RX_CBR1      0x81FF0008u
#define STEREO_RX_CTR3      0x03FE0000u
#define STEREO_RX_CBR2      0x07FE0000u
#define STEREO_NODE_LINK    0x38010000u
#define STEREO_CHANNEL_LINK 0xFE010000u
#define STEREO_HALF_TAG     0x00480000u
#define STEREO_COMPLETE_TAG 0x00540000u

#define CODEC_ADDRESS        (0x38u << 1)
#define CODEC_CLOCK_REGISTER 0x0Fu
#define TIMING_100K_AT_4M    0x00420F13u
#define CODEC_DRAIN_TICKS    1000u

uint32_t          received[DUPLEX_WORDS];
volatile uint32_t received_count;

static volatile uint32_t sent_count;
static volatile uint32_t underruns;
static volatile uint32_t status_seen;
static volatile uint32_t ring_blocks;
static volatile uint32_t ring_halves;
static volatile uint32_t received_blocks;

uint32_t ring[RING_WORDS];

int16_t const samples[SAMPLES] = {-3,  2,    -1,     0x7fff,  -32768, 0,  1, -2,
                                  100, -100, 0x1234, -0x1234, 7,      -7, 8, -8};

struct ring_node
{
  uint32_t br1;
  uint32_t sar;
  uint32_t llr;
};

static struct ring_node node;

struct stereo_list
{
  uint32_t head[8];
  uint32_t tail[4];
};

struct stereo_events
{
  uint32_t halves;
  uint32_t completes;
  uint32_t count;
  uint32_t at[STEREO_EVENTS];
};

struct stereo_path
{
  SAI_Block_TypeDef*   tx_block;
  SAI_Block_TypeDef*   rx_block;
  DMA_Channel_TypeDef* tx_channel;
  DMA_Channel_TypeDef* rx_channel;
  uint32_t             tx_ctr2;
  uint32_t             rx_ctr2;
  uint32_t             tx_cr1;
  IRQn_Type            tx_irq;
  IRQn_Type            rx_irq;
};

uint16_t tx_stereo[2][STEREO_FRAMES];
uint16_t rx_stereo[2][STEREO_FRAMES];

struct stereo_events stereo_tx_events;
struct stereo_events stereo_rx_events;
volatile uint32_t    stereo_start;

static struct stereo_list tx_list __attribute__((aligned(32)));
static struct stereo_list rx_list __attribute__((aligned(32)));

void idle(void);
void tx_poll(void);
void duplex_irq(void);
void underrun(void);
void SAI1_IRQHandler(void);
void dma(void);
void dma16_zero(void);
void dma16_sign(void);
void GPDMA1_Channel0_IRQHandler(void);
void GPDMA1_Channel1_IRQHandler(void);
void codec_dma(void);
void pair_dma(void);
void codec_i2c(void);
void GPDMA1_Channel12_IRQHandler(void);
void GPDMA1_Channel13_IRQHandler(void);
void GPDMA1_Channel14_IRQHandler(void);
void GPDMA1_Channel15_IRQHandler(void);
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

void GPDMA1_Channel0_IRQHandler(void)
{
  if(LL_DMA_IsActiveFlag_HT(GPDMA1, LL_DMA_CHANNEL_0))
  {
    LL_DMA_ClearFlag_HT(GPDMA1, LL_DMA_CHANNEL_0);
    ++ring_halves;
  }
  if(LL_DMA_IsActiveFlag_TC(GPDMA1, LL_DMA_CHANNEL_0))
  {
    LL_DMA_ClearFlag_TC(GPDMA1, LL_DMA_CHANNEL_0);
    ++ring_blocks;
  }
}

void GPDMA1_Channel1_IRQHandler(void)
{
  if(LL_DMA_IsActiveFlag_TC(GPDMA1, LL_DMA_CHANNEL_1))
  {
    LL_DMA_ClearFlag_TC(GPDMA1, LL_DMA_CHANNEL_1);
    ++received_blocks;
  }
}

static void ring_channel(void)
{
  node = (struct ring_node){
      RING_WORDS * 4u, (uint32_t)ring, LINK_UPDATE | ((uint32_t)&node & 0xfffcu)};
  LL_DMA_ConfigTransfer(
      GPDMA1, LL_DMA_CHANNEL_0,
      LL_DMA_SRC_INCREMENT
          | LL_DMA_DEST_FIXED
          | LL_DMA_SRC_DATAWIDTH_WORD
          | LL_DMA_DEST_DATAWIDTH_WORD);
  LL_DMA_SetDataTransferDirection(GPDMA1, LL_DMA_CHANNEL_0, LL_DMA_DIRECTION_MEMORY_TO_PERIPH);
  LL_DMA_SetPeriphRequest(GPDMA1, LL_DMA_CHANNEL_0, LL_GPDMA1_REQUEST_SAI1_A);
  LL_DMA_SetBlkDataLength(GPDMA1, LL_DMA_CHANNEL_0, RING_WORDS * 4u);
  LL_DMA_SetSrcAddress(GPDMA1, LL_DMA_CHANNEL_0, (uint32_t)ring);
  LL_DMA_SetDestAddress(GPDMA1, LL_DMA_CHANNEL_0, (uint32_t)&SAI1_Block_A->DR);
  LL_DMA_SetLinkedListBaseAddr(GPDMA1, LL_DMA_CHANNEL_0, (uint32_t)&node & 0xffff0000u);
  LL_DMA_ConfigLinkUpdate(GPDMA1, LL_DMA_CHANNEL_0, LINK_UPDATE, (uint32_t)&node);
  LL_DMA_EnableIT_HT(GPDMA1, LL_DMA_CHANNEL_0);
  LL_DMA_EnableIT_TC(GPDMA1, LL_DMA_CHANNEL_0);
  NVIC_EnableIRQ(GPDMA1_Channel0_IRQn);
}

static void receive_channel(void)
{
  LL_DMA_ConfigTransfer(
      GPDMA1, LL_DMA_CHANNEL_1,
      LL_DMA_SRC_FIXED
          | LL_DMA_DEST_INCREMENT
          | LL_DMA_SRC_DATAWIDTH_WORD
          | LL_DMA_DEST_DATAWIDTH_WORD);
  LL_DMA_SetDataTransferDirection(GPDMA1, LL_DMA_CHANNEL_1, LL_DMA_DIRECTION_PERIPH_TO_MEMORY);
  LL_DMA_SetPeriphRequest(GPDMA1, LL_DMA_CHANNEL_1, LL_GPDMA1_REQUEST_SAI1_B);
  LL_DMA_SetBlkDataLength(GPDMA1, LL_DMA_CHANNEL_1, DUPLEX_WORDS * 4u);
  LL_DMA_SetSrcAddress(GPDMA1, LL_DMA_CHANNEL_1, (uint32_t)&SAI1_Block_B->DR);
  LL_DMA_SetDestAddress(GPDMA1, LL_DMA_CHANNEL_1, (uint32_t)received);
  LL_DMA_EnableIT_TC(GPDMA1, LL_DMA_CHANNEL_1);
  NVIC_EnableIRQ(GPDMA1_Channel1_IRQn);
}

void dma(void)
{
  for(uint32_t index = 0; index < RING_WORDS; ++index)
  {
    ring[index] = ramp(index);
  }
  clocks();
  LL_AHB1_GRP1_EnableClock(LL_AHB1_GRP1_PERIPH_GPDMA1);
  configure(SAI1_Block_B, SAI_CR1_RX_SYNC_16 | SAI_xCR1_DMAEN, 0);
  configure(SAI1_Block_A, SAI_CR1_TX_MASTER_16 | SAI_xCR1_DMAEN, 0);
  ring_channel();
  receive_channel();
  LL_DMA_EnableChannel(GPDMA1, LL_DMA_CHANNEL_1);
  LL_DMA_EnableChannel(GPDMA1, LL_DMA_CHANNEL_0);
  SAI1_Block_B->CR1 |= SAI_xCR1_SAIEN;
  SAI1_Block_A->CR1 |= SAI_xCR1_SAIEN;
  while(ring_blocks < RING_BLOCKS)
  {
    __WFI();
  }
  disable(SAI1_Block_A);
  disable(SAI1_Block_B);
  LOG(ring_blocks);
  LOG(ring_halves);
  LOG(received_blocks);
  LOG(SAI1_Block_A->SR & SAI_xSR_OVRUDR);
  DONE();
}

static void dma16(uint32_t alignment)
{
  clocks();
  LL_AHB1_GRP1_EnableClock(LL_AHB1_GRP1_PERIPH_GPDMA1);
  configure(SAI1_Block_A, SAI_CR1_TX_MASTER_32 | SAI_xCR1_DMAEN, 0);
  LL_DMA_ConfigTransfer(
      GPDMA1, LL_DMA_CHANNEL_0,
      LL_DMA_SRC_INCREMENT
          | LL_DMA_DEST_FIXED
          | LL_DMA_SRC_DATAWIDTH_HALFWORD
          | LL_DMA_DEST_DATAWIDTH_WORD
          | alignment);
  LL_DMA_SetDataTransferDirection(GPDMA1, LL_DMA_CHANNEL_0, LL_DMA_DIRECTION_MEMORY_TO_PERIPH);
  LL_DMA_SetPeriphRequest(GPDMA1, LL_DMA_CHANNEL_0, LL_GPDMA1_REQUEST_SAI1_A);
  LL_DMA_SetBlkDataLength(GPDMA1, LL_DMA_CHANNEL_0, SAMPLES * 2u);
  LL_DMA_SetSrcAddress(GPDMA1, LL_DMA_CHANNEL_0, (uint32_t)samples);
  LL_DMA_SetDestAddress(GPDMA1, LL_DMA_CHANNEL_0, (uint32_t)&SAI1_Block_A->DR);
  LL_DMA_EnableIT_TC(GPDMA1, LL_DMA_CHANNEL_0);
  NVIC_EnableIRQ(GPDMA1_Channel0_IRQn);
  LL_DMA_EnableChannel(GPDMA1, LL_DMA_CHANNEL_0);
  SAI1_Block_A->CR1 |= SAI_xCR1_SAIEN;
  while(ring_blocks == 0)
  {
    __WFI();
  }
  while((SAI1_Block_A->SR & SAI_xSR_FLVL) != 0)
  {
  }
  disable(SAI1_Block_A);
  LOG(ring_blocks);
  LOG(SAI1_Block_A->SR & SAI_xSR_OVRUDR);
  DONE();
}

void dma16_zero(void)
{
  dma16(LL_DMA_DATA_ALIGN_ZEROPADD);
}

void dma16_sign(void)
{
  dma16(LL_DMA_DATA_ALIGN_SIGNEXTPADD);
}

static void stereo_event(DMA_Channel_TypeDef* channel, struct stereo_events* events, uint32_t tag)
{
  uint32_t const now = TIM2->CNT;
  if((channel->CSR & DMA_CSR_HTF) != 0)
  {
    channel->CFCR = DMA_CFCR_HTF;
    LOG(tag | STEREO_HALF_TAG | ++events->halves);
    if(events->count < STEREO_EVENTS)
    {
      events->at[events->count++] = now;
    }
  }
  if((channel->CSR & DMA_CSR_TCF) != 0)
  {
    channel->CFCR = DMA_CFCR_TCF;
    LOG(tag | STEREO_COMPLETE_TAG | ++events->completes);
    if(events->count < STEREO_EVENTS)
    {
      events->at[events->count++] = now;
    }
  }
}

void GPDMA1_Channel12_IRQHandler(void)
{
  stereo_event(GPDMA1_Channel12, &stereo_tx_events, 0x0C000000u);
}

void GPDMA1_Channel13_IRQHandler(void)
{
  stereo_event(GPDMA1_Channel13, &stereo_rx_events, 0x0D000000u);
}

void GPDMA1_Channel14_IRQHandler(void)
{
  stereo_event(GPDMA1_Channel14, &stereo_tx_events, 0x0E000000u);
}

void GPDMA1_Channel15_IRQHandler(void)
{
  stereo_event(GPDMA1_Channel15, &stereo_rx_events, 0x0F000000u);
}

static void free_running_tim2(void)
{
  RCC->APB1ENR1 |= RCC_APB1ENR1_TIM2EN;
  (void)RCC->APB1ENR1;
  TIM2->PSC = 3u;
  TIM2->ARR = 0xFFFFFFFFu;
  TIM2->EGR = TIM_EGR_UG;
  TIM2->SR  = 0u;
  TIM2->CR1 = TIM_CR1_CEN;
}

static void stereo_block(SAI_Block_TypeDef* block, uint32_t cr1)
{
  block->CR1   = cr1;
  block->CR2   = SAI_xCR2_FFLUSH;
  block->FRCR  = STEREO_FRCR;
  block->SLOTR = STEREO_SLOTR;
  block->IMR   = STEREO_IMR;
}

static void stereo_channel(
    DMA_Channel_TypeDef* channel,
    struct stereo_list*  list,
    uint32_t const       head[7],
    uint32_t             tail_br1,
    IRQn_Type            irq)
{
  uint32_t const node_link = STEREO_NODE_LINK | ((uint32_t)list->tail & 0xFFFCu);
  for(uint32_t index = 0; index < 7u; ++index)
  {
    list->head[index] = head[index];
  }
  list->head[7]  = node_link;
  list->tail[0]  = tail_br1;
  list->tail[1]  = head[3];
  list->tail[2]  = head[4];
  list->tail[3]  = node_link;
  channel->CTR2  = DMA_CTR2_TCEM_0;
  channel->CBR1  = 0u;
  channel->CLBAR = (uint32_t)list->head & 0xFFFF0000u;
  channel->CLLR  = STEREO_CHANNEL_LINK | ((uint32_t)list->head & 0xFFFCu);
  channel->CCR   = STEREO_CCR;
  NVIC_EnableIRQ(irq);
}

static uint32_t checksum(uint16_t const buffer[2][STEREO_FRAMES])
{
  uint32_t sum = 0;
  for(uint32_t index = 0; index < STEREO_FRAMES; ++index)
  {
    sum = sum * 31u + buffer[0][index];
    sum = sum * 31u + buffer[1][index];
  }
  return sum;
}

static void stereo_begin(struct stereo_path const* path)
{
  for(uint32_t index = 0; index < STEREO_FRAMES; ++index)
  {
    tx_stereo[0][index] = (uint16_t)index;
    tx_stereo[1][index] = (uint16_t)(0x8000u | index);
  }
  free_running_tim2();
  LL_AHB1_GRP1_EnableClock(LL_AHB1_GRP1_PERIPH_GPDMA1);
  stereo_block(path->tx_block, path->tx_cr1);
  stereo_block(path->rx_block, STEREO_RX_CR1_SYNC);
  uint32_t const tx_head[7] = {STEREO_TX_CTR1,
                               path->tx_ctr2,
                               STEREO_TX_CBR1,
                               (uint32_t)tx_stereo,
                               (uint32_t)&path->tx_block->DR,
                               STEREO_TX_CTR3,
                               STEREO_TX_CBR2};
  uint32_t const rx_head[7] = {
      STEREO_RX_CTR1,      path->rx_ctr2,  STEREO_RX_CBR1, (uint32_t)&path->rx_block->DR,
      (uint32_t)rx_stereo, STEREO_RX_CTR3, STEREO_RX_CBR2};
  stereo_channel(path->tx_channel, &tx_list, tx_head, STEREO_TX_CBR1, path->tx_irq);
  stereo_channel(path->rx_channel, &rx_list, rx_head, STEREO_RX_CBR1, path->rx_irq);
  path->tx_channel->CCR |= DMA_CCR_EN;
  path->tx_block->CR1 |= SAI_xCR1_DMAEN;
  while((path->tx_block->SR & SAI_xSR_FLVL) == 0)
  {
  }
  path->tx_block->CR1 |= SAI_xCR1_SAIEN;
  stereo_start = TIM2->CNT;
  path->rx_channel->CCR |= DMA_CCR_EN;
  path->rx_block->CR1 |= SAI_xCR1_DMAEN;
  path->rx_block->CR1 |= SAI_xCR1_SAIEN;
}

static void stereo_end(struct stereo_path const* path)
{
  path->rx_channel->CCR |= DMA_CCR_SUSP;
  path->tx_channel->CCR |= DMA_CCR_SUSP;
  LOG(checksum(tx_stereo));
  LOG(checksum(rx_stereo));
  DONE();
}

static void stereo(struct stereo_path const* path)
{
  stereo_begin(path);
  while(stereo_tx_events.halves < STEREO_HALVES || stereo_rx_events.halves < STEREO_HALVES)
  {
    __WFI();
  }
  stereo_end(path);
}

void codec_dma(void)
{
  struct stereo_path const path = {
      SAI1_Block_A, SAI1_Block_B,        GPDMA1_Channel12,      GPDMA1_Channel13,      0x80000424u,
      0x80000025u,  STEREO_TX_CR1_SLAVE, GPDMA1_Channel12_IRQn, GPDMA1_Channel13_IRQn,
  };
  clocks();
  stereo(&path);
}

void pair_dma(void)
{
  struct stereo_path const path = {
      SAI2_Block_A, SAI2_Block_B,         GPDMA1_Channel14,      GPDMA1_Channel15,      0x80000426u,
      0x80000027u,  STEREO_TX_CR1_MASTER, GPDMA1_Channel14_IRQn, GPDMA1_Channel15_IRQn,
  };
  RCC->CR |= RCC_CR_HSION;
  while((RCC->CR & RCC_CR_HSIRDY) == 0)
  {
  }
  RCC->CCIPR2 = (RCC->CCIPR2 & ~RCC_CCIPR2_SAI2SEL) | (4u << RCC_CCIPR2_SAI2SEL_Pos);
  RCC->APB2ENR |= RCC_APB2ENR_SAI2EN;
  (void)RCC->APB2ENR;
  stereo(&path);
}

static void codec_clock(uint8_t on)
{
  uint8_t const command[2] = {CODEC_CLOCK_REGISTER, on};
  LL_I2C_HandleTransfer(
      I2C1, CODEC_ADDRESS, LL_I2C_ADDRSLAVE_7BIT, 2u, LL_I2C_MODE_AUTOEND,
      LL_I2C_GENERATE_START_WRITE);
  for(uint32_t index = 0; index < 2u; ++index)
  {
    while(!LL_I2C_IsActiveFlag_TXIS(I2C1))
    {
    }
    LL_I2C_TransmitData8(I2C1, command[index]);
  }
  while(!LL_I2C_IsActiveFlag_STOP(I2C1))
  {
  }
  LL_I2C_ClearFlag_STOP(I2C1);
}

void codec_i2c(void)
{
  struct stereo_path const path = {
      SAI1_Block_A, SAI1_Block_B,        GPDMA1_Channel12,      GPDMA1_Channel13,      0x80000424u,
      0x80000025u,  STEREO_TX_CR1_SLAVE, GPDMA1_Channel12_IRQn, GPDMA1_Channel13_IRQn,
  };
  LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_I2C1);
  LL_I2C_InitTypeDef const i2c = {
      .PeripheralMode  = LL_I2C_MODE_I2C,
      .Timing          = TIMING_100K_AT_4M,
      .AnalogFilter    = LL_I2C_ANALOGFILTER_ENABLE,
      .DigitalFilter   = 0,
      .OwnAddress1     = 0,
      .TypeAcknowledge = LL_I2C_ACK,
      .OwnAddrSize     = LL_I2C_OWNADDRESS1_7BIT,
  };
  LL_I2C_Init(I2C1, &i2c);
  clocks();
  stereo_begin(&path);
  codec_clock(1u);
  while(stereo_rx_events.halves < STEREO_HALVES)
  {
    __WFI();
  }
  codec_clock(0u);
  uint32_t const stopped = TIM2->CNT;
  while(TIM2->CNT - stopped < CODEC_DRAIN_TICKS)
  {
  }
  stereo_end(&path);
}

int main(void)
{
  idle();
}
