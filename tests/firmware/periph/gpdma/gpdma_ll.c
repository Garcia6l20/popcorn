#include "periph_test.h"
#include "stm32u5xx_ll_bus.h"
#include "stm32u5xx_ll_dma.h"

#define WORD_COPY              \
  (LL_DMA_SRC_INCREMENT        \
   | LL_DMA_DEST_INCREMENT     \
   | LL_DMA_SRC_DATAWIDTH_WORD \
   | LL_DMA_DEST_DATAWIDTH_WORD)

uint8_t source[256];
uint8_t destination[256];
uint8_t items_out[3][64];

struct lli
{
  uint32_t br1;
  uint32_t sar;
  uint32_t dar;
  uint32_t llr;
};

struct lli items[2];

static volatile uint32_t completions;
static volatile uint32_t halves;
static volatile uint32_t errors;

void GPDMA1_Channel0_IRQHandler(void);
void GPDMA1_Channel1_IRQHandler(void);
void mem2mem(void);
void linked(void);
void error(void);
void suspend(void);
void half(void);

void GPDMA1_Channel0_IRQHandler(void)
{
  if(LL_DMA_IsActiveFlag_HT(GPDMA1, LL_DMA_CHANNEL_0))
  {
    LL_DMA_ClearFlag_HT(GPDMA1, LL_DMA_CHANNEL_0);
    ++halves;
  }
  if(LL_DMA_IsActiveFlag_TC(GPDMA1, LL_DMA_CHANNEL_0))
  {
    LL_DMA_ClearFlag_TC(GPDMA1, LL_DMA_CHANNEL_0);
    ++completions;
  }
  if(LL_DMA_IsActiveFlag_DTE(GPDMA1, LL_DMA_CHANNEL_0))
  {
    LL_DMA_ClearFlag_DTE(GPDMA1, LL_DMA_CHANNEL_0);
    ++errors;
  }
}

void GPDMA1_Channel1_IRQHandler(void)
{
  if(LL_DMA_IsActiveFlag_TC(GPDMA1, LL_DMA_CHANNEL_1))
  {
    LL_DMA_ClearFlag_TC(GPDMA1, LL_DMA_CHANNEL_1);
    ++completions;
  }
}

static void fill(void)
{
  for(uint32_t index = 0; index < sizeof source; ++index)
  {
    source[index]      = (uint8_t)(index * 7u + 3u);
    destination[index] = 0;
  }
}

static uint32_t same(uint8_t const* a, uint8_t const* b, uint32_t length)
{
  for(uint32_t index = 0; index < length; ++index)
  {
    if(a[index] != b[index])
    {
      return 0;
    }
  }
  return 1;
}

static void copy_channel0(uint32_t length, uint32_t source_address)
{
  LL_AHB1_GRP1_EnableClock(LL_AHB1_GRP1_PERIPH_GPDMA1);
  LL_DMA_ResetChannel(GPDMA1, LL_DMA_CHANNEL_0);
  LL_DMA_ConfigTransfer(GPDMA1, LL_DMA_CHANNEL_0, WORD_COPY);
  LL_DMA_SetDataTransferDirection(GPDMA1, LL_DMA_CHANNEL_0, LL_DMA_DIRECTION_MEMORY_TO_MEMORY);
  LL_DMA_SetBlkDataLength(GPDMA1, LL_DMA_CHANNEL_0, length);
  LL_DMA_SetSrcAddress(GPDMA1, LL_DMA_CHANNEL_0, source_address);
  LL_DMA_SetDestAddress(GPDMA1, LL_DMA_CHANNEL_0, (uint32_t)destination);
  LL_DMA_ConfigLinkUpdate(GPDMA1, LL_DMA_CHANNEL_0, 0, 0);
  LL_DMA_EnableIT_TC(GPDMA1, LL_DMA_CHANNEL_0);
  LL_DMA_EnableIT_DTE(GPDMA1, LL_DMA_CHANNEL_0);
  NVIC_EnableIRQ(GPDMA1_Channel0_IRQn);
}

void mem2mem(void)
{
  fill();
  copy_channel0(sizeof source, (uint32_t)source);
  LL_DMA_EnableChannel(GPDMA1, LL_DMA_CHANNEL_0);
  while(completions == 0)
  {
  }
  LOG(completions);
  LOG(same(source, destination, sizeof source));
  LOG(LL_DMA_IsEnabledChannel(GPDMA1, LL_DMA_CHANNEL_0));
  DONE();
}

void half(void)
{
  fill();
  copy_channel0(64, (uint32_t)source);
  LL_DMA_EnableIT_HT(GPDMA1, LL_DMA_CHANNEL_0);
  LL_DMA_EnableChannel(GPDMA1, LL_DMA_CHANNEL_0);
  while(completions == 0)
  {
  }
  LOG(halves);
  LOG(completions);
  LOG(same(source, destination, 64));
  DONE();
}

void error(void)
{
  copy_channel0(16, 0x60000000u);
  LL_DMA_EnableChannel(GPDMA1, LL_DMA_CHANNEL_0);
  while(errors == 0)
  {
  }
  LOG(errors);
  LOG(LL_DMA_IsEnabledChannel(GPDMA1, LL_DMA_CHANNEL_0));
  LOG(LL_DMA_IsActiveFlag_IDLE(GPDMA1, LL_DMA_CHANNEL_0));
  DONE();
}

void suspend(void)
{
  fill();
  copy_channel0(sizeof source, (uint32_t)source);
  LL_DMA_EnableChannel(GPDMA1, LL_DMA_CHANNEL_0);
  LL_DMA_SuspendChannel(GPDMA1, LL_DMA_CHANNEL_0);
  while(!LL_DMA_IsActiveFlag_SUSP(GPDMA1, LL_DMA_CHANNEL_0))
  {
  }
  LOG(same(source, destination, sizeof source));
  LL_DMA_ClearFlag_SUSP(GPDMA1, LL_DMA_CHANNEL_0);
  LL_DMA_ResumeChannel(GPDMA1, LL_DMA_CHANNEL_0);
  while(completions == 0)
  {
  }
  LOG(same(source, destination, sizeof source));
  DONE();
}

void linked(void)
{
  fill();
  uint32_t const base = (uint32_t)items & 0xffff0000u;
  uint32_t const update =
      LL_DMA_UPDATE_CBR1 | LL_DMA_UPDATE_CSAR | LL_DMA_UPDATE_CDAR | LL_DMA_UPDATE_CLLR;
  items[0] = (struct lli){
      32, (uint32_t)source + 64, (uint32_t)items_out[1], update | ((uint32_t)&items[1] & 0xfffcu)};
  items[1] = (struct lli){48, (uint32_t)source + 128, (uint32_t)items_out[2], 0};
  LL_AHB1_GRP1_EnableClock(LL_AHB1_GRP1_PERIPH_GPDMA1);
  LL_DMA_ConfigTransfer(GPDMA1, LL_DMA_CHANNEL_1, WORD_COPY);
  LL_DMA_SetDataTransferDirection(GPDMA1, LL_DMA_CHANNEL_1, LL_DMA_DIRECTION_MEMORY_TO_MEMORY);
  LL_DMA_SetTransferEventMode(GPDMA1, LL_DMA_CHANNEL_1, LL_DMA_TCEM_EACH_LLITEM_TRANSFER);
  LL_DMA_SetBlkDataLength(GPDMA1, LL_DMA_CHANNEL_1, 16);
  LL_DMA_SetSrcAddress(GPDMA1, LL_DMA_CHANNEL_1, (uint32_t)source);
  LL_DMA_SetDestAddress(GPDMA1, LL_DMA_CHANNEL_1, (uint32_t)items_out[0]);
  LL_DMA_SetLinkedListBaseAddr(GPDMA1, LL_DMA_CHANNEL_1, base);
  LL_DMA_ConfigLinkUpdate(GPDMA1, LL_DMA_CHANNEL_1, update, (uint32_t)&items[0]);
  LL_DMA_EnableIT_TC(GPDMA1, LL_DMA_CHANNEL_1);
  NVIC_EnableIRQ(GPDMA1_Channel1_IRQn);
  LL_DMA_EnableChannel(GPDMA1, LL_DMA_CHANNEL_1);
  while(LL_DMA_IsEnabledChannel(GPDMA1, LL_DMA_CHANNEL_1))
  {
  }
  __DSB();
  LOG(completions);
  LOG(same(source, items_out[0], 16));
  LOG(same(source + 64, items_out[1], 32));
  LOG(same(source + 128, items_out[2], 48));
  LOG(LL_DMA_IsActiveFlag_IDLE(GPDMA1, LL_DMA_CHANNEL_1));
  DONE();
}

int main(void)
{
  mem2mem();
}
