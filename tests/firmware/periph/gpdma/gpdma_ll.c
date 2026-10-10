#include "periph_test.h"
#include "stm32u5xx_ll_bus.h"
#include "stm32u5xx_ll_dma.h"
#include "stm32u5xx_ll_rcc.h"
#include "stm32u5xx_ll_spi.h"
#include "stm32u5xx_ll_usart.h"

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
static volatile uint32_t done;

void GPDMA1_Channel0_IRQHandler(void);
void GPDMA1_Channel1_IRQHandler(void);
void mem2mem(void);
void linked(void);
void error(void);
void suspend(void);
void half(void);
void two_d_hw_request(void);
void two_d_hw_request_brc1(void);
void GPDMA1_Channel15_IRQHandler(void);

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

void GPDMA1_Channel15_IRQHandler(void)
{
  DMA_Channel_TypeDef* const channel = GPDMA1_Channel15;
  if((channel->CSR & DMA_CSR_TCF) != 0)
  {
    LOG(channel->CSR);
    LOG(channel->CCR);
    LOG(channel->CBR1);
    LOG(channel->CSAR - (uint32_t)source);
    channel->CFCR = DMA_CFCR_TCF | DMA_CFCR_HTF;
    done          = 1;
  }
}

void two_d_hw_request(void)
{
  DMA_Channel_TypeDef* const channel = GPDMA1_Channel15;
  fill();
  LL_APB2_GRP1_EnableClock(LL_APB2_GRP1_PERIPH_USART1);
  LL_RCC_SetUSARTClockSource(LL_RCC_USART1_CLKSOURCE_PCLK2);
  LL_USART_InitTypeDef config = {
      .PrescalerValue      = LL_USART_PRESCALER_DIV1,
      .BaudRate            = 115200,
      .DataWidth           = LL_USART_DATAWIDTH_8B,
      .StopBits            = LL_USART_STOPBITS_1,
      .Parity              = LL_USART_PARITY_NONE,
      .TransferDirection   = LL_USART_DIRECTION_TX,
      .HardwareFlowControl = LL_USART_HWCONTROL_NONE,
      .OverSampling        = LL_USART_OVERSAMPLING_16,
  };
  LL_USART_Init(USART1, &config);
  LL_USART_ConfigAsyncMode(USART1);
  LL_USART_Enable(USART1);
  while(!LL_USART_IsActiveFlag_TEACK(USART1))
  {
  }
  LL_AHB1_GRP1_EnableClock(LL_AHB1_GRP1_PERIPH_GPDMA1);
  channel->CTR1 = DMA_CTR1_SINC;
  channel->CTR2 = LL_DMA_TCEM_RPT_BLK_TRANSFER | DMA_CTR2_DREQ | LL_GPDMA1_REQUEST_USART1_TX;
  channel->CBR1 = 100;
  channel->CSAR = (uint32_t)source;
  channel->CDAR = (uint32_t)&USART1->TDR;
  channel->CLLR = 0;
  NVIC_EnableIRQ(GPDMA1_Channel15_IRQn);
  channel->CCR = DMA_CCR_LAP | DMA_CCR_USEIE | DMA_CCR_ULEIE | DMA_CCR_DTEIE | DMA_CCR_TCIE;
  channel->CCR |= DMA_CCR_EN;
  LL_USART_EnableDMAReq_TX(USART1);
  while(done == 0)
  {
  }
  LOG(done);
  DONE();
}

void two_d_hw_request_brc1(void)
{
  DMA_Channel_TypeDef* const channel = GPDMA1_Channel15;
  fill();
  LL_APB2_GRP1_EnableClock(LL_APB2_GRP1_PERIPH_SPI1);
  LL_SPI_InitTypeDef config = {
      .TransferDirection = LL_SPI_SIMPLEX_TX,
      .Mode              = LL_SPI_MODE_MASTER,
      .DataWidth         = LL_SPI_DATAWIDTH_8BIT,
      .ClockPolarity     = LL_SPI_POLARITY_LOW,
      .ClockPhase        = LL_SPI_PHASE_1EDGE,
      .NSS               = LL_SPI_NSS_SOFT,
      .BaudRate          = LL_SPI_BAUDRATEPRESCALER_DIV2,
      .BitOrder          = LL_SPI_MSB_FIRST,
      .CRCCalculation    = LL_SPI_CRCCALCULATION_DISABLE,
      .CRCPoly           = 7,
  };
  LL_SPI_Init(SPI1, &config);
  LL_SPI_SetInternalSSLevel(SPI1, LL_SPI_SS_LEVEL_HIGH);
  LL_SPI_SetFIFOThreshold(SPI1, LL_SPI_FIFO_TH_01DATA);
  LL_SPI_SetTransferSize(SPI1, 100);
  LL_AHB1_GRP1_EnableClock(LL_AHB1_GRP1_PERIPH_GPDMA1);
  channel->CTR1 = DMA_CTR1_SINC;
  channel->CTR2 = LL_DMA_TCEM_RPT_BLK_TRANSFER | DMA_CTR2_DREQ | LL_GPDMA1_REQUEST_SPI1_TX;
  channel->CBR1 = 1u << DMA_CBR1_BRC_Pos;
  LL_DMA_SetBlkDataLength(GPDMA1, LL_DMA_CHANNEL_15, 100);
  channel->CSAR = (uint32_t)source;
  channel->CDAR = (uint32_t)&SPI1->TXDR;
  channel->CLLR = 0;
  NVIC_EnableIRQ(GPDMA1_Channel15_IRQn);
  channel->CCR = DMA_CCR_LAP | DMA_CCR_USEIE | DMA_CCR_ULEIE | DMA_CCR_DTEIE | DMA_CCR_TCIE;
  channel->CCR |= DMA_CCR_EN;
  LL_SPI_EnableDMAReq_TX(SPI1);
  LL_SPI_Enable(SPI1);
  LL_SPI_StartMasterTransfer(SPI1);
  while(!LL_SPI_IsActiveFlag_EOT(SPI1))
  {
  }
  for(volatile uint32_t left = 5000; left != 0; --left)
  {
  }
  LOG(channel->CSR);
  LOG(channel->CCR);
  LOG(channel->CBR1);
  LOG(channel->CSAR - (uint32_t)source);
  LOG(done);
  DONE();
}

int main(void)
{
  mem2mem();
}
