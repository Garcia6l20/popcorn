#include "periph_test.h"
#include "stm32u5xx_ll_bus.h"
#include "stm32u5xx_ll_dma.h"
#include "stm32u5xx_ll_rcc.h"
#include "stm32u5xx_ll_tim.h"

static volatile uint32_t updates;
static volatile uint32_t matches;
static volatile uint32_t dma_done;
static uint32_t const    reloads[4] = {999, 999, 999, 999};
static uint32_t const    pattern[8] = {1u, 2u, 4u, 8u, 1u << 16, 2u << 16, 4u << 16, 8u << 16};
static uint32_t const    thirds[4]  = {11, 12, 13, 14};
static uint32_t const    fourths[4] = {21, 22, 23, 24};
static volatile uint32_t channel_done[3];
static uint32_t const    nine[9] = {1, 2, 3, 4, 5, 6, 7, 8, 9};

void TIM16_IRQHandler(void);
void TIM1_UP_IRQHandler(void);
void TIM2_IRQHandler(void);
void TIM3_IRQHandler(void);
void TIM6_IRQHandler(void);
void GPDMA1_Channel0_IRQHandler(void);
void GPDMA1_Channel1_IRQHandler(void);
void GPDMA1_Channel2_IRQHandler(void);
void idle(void);
void tim16_updates(void);
void tim16_one_pulse(void);
void tim2_compare(void);
void tim6_preload(void);
void tim6_clock(void);
void tim6_dma(void);
void tim6_poll(void);
void tim4_triggers_tim5(void);
void tim1_clocks_tim15(void);
void tim15_unclocked(void);
void tim3_encoder_idle(void);
void tim6_dma_bsrr(void);
void tim2_two_requests(void);
void tim5_burst_cpu(void);
void tim5_burst_dma(void);

void idle(void)
{
  for(;;)
  {
    __WFI();
  }
}

static void wait(uint32_t loops)
{
  for(volatile uint32_t left = loops; left != 0; --left)
  {
  }
}

static void clock_160mhz(void)
{
  LL_RCC_MSIS_Enable();
  while(!LL_RCC_MSIS_IsReady())
  {
  }
  LL_RCC_PLL1_ConfigDomain_SYS(LL_RCC_PLL1SOURCE_MSIS, 1, 80, 2);
  LL_RCC_PLL1_SetVCOInputRange(LL_RCC_PLLINPUTRANGE_4_8);
  LL_RCC_PLL1_EnableDomain_SYS();
  LL_RCC_PLL1_Enable();
  while(!LL_RCC_PLL1_IsReady())
  {
  }
  LL_RCC_SetAHBPrescaler(LL_RCC_SYSCLK_DIV_1);
  LL_RCC_SetAPB1Prescaler(LL_RCC_APB1_DIV_1);
  LL_RCC_SetAPB2Prescaler(LL_RCC_APB2_DIV_1);
  LL_RCC_SetSysClkSource(LL_RCC_SYS_CLKSOURCE_PLL1);
  while(LL_RCC_GetSysClkSource() != LL_RCC_SYS_CLKSOURCE_STATUS_PLL1)
  {
  }
}

static void time_base(TIM_TypeDef* tim, uint32_t prescaler, uint32_t reload)
{
  LL_TIM_InitTypeDef config = {
      .Prescaler         = prescaler,
      .CounterMode       = LL_TIM_COUNTERMODE_UP,
      .Autoreload        = reload,
      .ClockDivision     = LL_TIM_CLOCKDIVISION_DIV1,
      .RepetitionCounter = 0,
  };
  LL_TIM_Init(tim, &config);
  LL_TIM_ClearFlag_UPDATE(tim);
}

void TIM16_IRQHandler(void)
{
  if(LL_TIM_IsActiveFlag_UPDATE(TIM16))
  {
    LL_TIM_ClearFlag_UPDATE(TIM16);
    ++updates;
  }
}

void TIM1_UP_IRQHandler(void)
{
  if(LL_TIM_IsActiveFlag_UPDATE(TIM1))
  {
    LL_TIM_ClearFlag_UPDATE(TIM1);
    ++updates;
  }
}

void TIM2_IRQHandler(void)
{
  if(LL_TIM_IsActiveFlag_CC1(TIM2))
  {
    LOG(LL_TIM_GetCounter(TIM2));
    LL_TIM_ClearFlag_CC1(TIM2);
    ++matches;
  }
}

void TIM3_IRQHandler(void)
{
  if(LL_TIM_IsActiveFlag_UPDATE(TIM3))
  {
    LL_TIM_ClearFlag_UPDATE(TIM3);
    ++updates;
  }
}

void TIM6_IRQHandler(void)
{
  if(!LL_TIM_IsActiveFlag_UPDATE(TIM6))
  {
    return;
  }
  LL_TIM_ClearFlag_UPDATE(TIM6);
  ++updates;
  if(LL_TIM_IsEnabledARRPreload(TIM6) && updates == 3)
  {
    LL_TIM_SetPrescaler(TIM6, 7);
  }
  if(LL_TIM_IsEnabledARRPreload(TIM6) && updates == 6)
  {
    LL_TIM_SetAutoReload(TIM6, 499);
  }
}

void GPDMA1_Channel0_IRQHandler(void)
{
  if(LL_DMA_IsActiveFlag_TC(GPDMA1, LL_DMA_CHANNEL_0))
  {
    LL_DMA_ClearFlag_TC(GPDMA1, LL_DMA_CHANNEL_0);
    LOG(updates);
    dma_done = 1;
  }
}

void GPDMA1_Channel1_IRQHandler(void)
{
  if(LL_DMA_IsActiveFlag_TC(GPDMA1, LL_DMA_CHANNEL_1))
  {
    LL_DMA_ClearFlag_TC(GPDMA1, LL_DMA_CHANNEL_1);
    channel_done[1] = 1;
  }
}

void GPDMA1_Channel2_IRQHandler(void)
{
  if(LL_DMA_IsActiveFlag_TC(GPDMA1, LL_DMA_CHANNEL_2))
  {
    LL_DMA_ClearFlag_TC(GPDMA1, LL_DMA_CHANNEL_2);
    channel_done[2] = 1;
  }
}

static void word_channel(
    uint32_t        channel,
    uint32_t        request,
    uint32_t const* source,
    uint32_t        words,
    uint32_t        destination)
{
  LL_DMA_ConfigTransfer(
      GPDMA1, channel,
      LL_DMA_SRC_INCREMENT
          | LL_DMA_DEST_FIXED
          | LL_DMA_SRC_DATAWIDTH_WORD
          | LL_DMA_DEST_DATAWIDTH_WORD);
  LL_DMA_SetDataTransferDirection(GPDMA1, channel, LL_DMA_DIRECTION_MEMORY_TO_PERIPH);
  LL_DMA_SetPeriphRequest(GPDMA1, channel, request);
  LL_DMA_SetBlkDataLength(GPDMA1, channel, 4 * words);
  LL_DMA_SetSrcAddress(GPDMA1, channel, (uint32_t)source);
  LL_DMA_SetDestAddress(GPDMA1, channel, destination);
  LL_DMA_EnableIT_TC(GPDMA1, channel);
}

void tim16_updates(void)
{
  clock_160mhz();
  LL_APB2_GRP1_EnableClock(LL_APB2_GRP1_PERIPH_TIM16);
  time_base(TIM16, 159, 999);
  LL_TIM_EnableIT_UPDATE(TIM16);
  NVIC_EnableIRQ(TIM16_IRQn);
  LL_TIM_EnableCounter(TIM16);
  while(updates < 100)
  {
    __WFI();
  }
  LL_TIM_DisableCounter(TIM16);
  LOG(updates);
  DONE();
}

void tim16_one_pulse(void)
{
  clock_160mhz();
  LL_APB2_GRP1_EnableClock(LL_APB2_GRP1_PERIPH_TIM16);
  time_base(TIM16, 159, 999);
  LL_TIM_SetOnePulseMode(TIM16, LL_TIM_ONEPULSEMODE_SINGLE);
  LL_TIM_EnableIT_UPDATE(TIM16);
  NVIC_EnableIRQ(TIM16_IRQn);
  LL_TIM_EnableCounter(TIM16);
  while(LL_TIM_IsEnabledCounter(TIM16))
  {
  }
  wait(100000);
  LOG(updates);
  LOG(LL_TIM_IsEnabledCounter(TIM16));
  LOG(LL_TIM_GetCounter(TIM16));
  DONE();
}

void tim2_compare(void)
{
  LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_TIM2);
  time_base(TIM2, 0, 0xffffffffu);
  LL_TIM_OC_SetMode(TIM2, LL_TIM_CHANNEL_CH1, LL_TIM_OCMODE_FROZEN);
  LL_TIM_OC_SetCompareCH1(TIM2, 40000);
  LL_TIM_ClearFlag_CC1(TIM2);
  LL_TIM_EnableIT_CC1(TIM2);
  NVIC_EnableIRQ(TIM2_IRQn);
  LL_TIM_EnableCounter(TIM2);
  while(matches == 0)
  {
    __WFI();
  }
  wait(20000);
  LOG(matches);
  DONE();
}

void tim6_preload(void)
{
  LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_TIM6);
  time_base(TIM6, 3, 999);
  LL_TIM_EnableARRPreload(TIM6);
  LL_TIM_EnableIT_UPDATE(TIM6);
  NVIC_EnableIRQ(TIM6_IRQn);
  LL_TIM_EnableCounter(TIM6);
  while(updates < 10)
  {
    __WFI();
  }
  LOG(updates);
  DONE();
}

void tim6_clock(void)
{
  LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_TIM6);
  time_base(TIM6, 3, 999);
  LL_TIM_EnableIT_UPDATE(TIM6);
  NVIC_EnableIRQ(TIM6_IRQn);
  LL_TIM_EnableCounter(TIM6);
  while(updates < 2)
  {
    __WFI();
  }
  while(LL_TIM_GetCounter(TIM6) < 500)
  {
  }
  LL_RCC_SetAHBPrescaler(LL_RCC_SYSCLK_DIV_2);
  while(updates < 5)
  {
    __WFI();
  }
  LOG(updates);
  DONE();
}

void tim6_dma(void)
{
  LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_TIM6);
  LL_AHB1_GRP1_EnableClock(LL_AHB1_GRP1_PERIPH_GPDMA1);
  time_base(TIM6, 3, 999);
  LL_DMA_ConfigTransfer(
      GPDMA1, LL_DMA_CHANNEL_0,
      LL_DMA_SRC_INCREMENT
          | LL_DMA_DEST_FIXED
          | LL_DMA_SRC_DATAWIDTH_WORD
          | LL_DMA_DEST_DATAWIDTH_WORD);
  LL_DMA_SetDataTransferDirection(GPDMA1, LL_DMA_CHANNEL_0, LL_DMA_DIRECTION_MEMORY_TO_PERIPH);
  LL_DMA_SetPeriphRequest(GPDMA1, LL_DMA_CHANNEL_0, LL_GPDMA1_REQUEST_TIM6_UP);
  LL_DMA_SetBlkDataLength(GPDMA1, LL_DMA_CHANNEL_0, sizeof reloads);
  LL_DMA_SetSrcAddress(GPDMA1, LL_DMA_CHANNEL_0, (uint32_t)reloads);
  LL_DMA_SetDestAddress(GPDMA1, LL_DMA_CHANNEL_0, (uint32_t)&TIM6->ARR);
  LL_DMA_EnableIT_TC(GPDMA1, LL_DMA_CHANNEL_0);
  NVIC_EnableIRQ(GPDMA1_Channel0_IRQn);
  LL_DMA_EnableChannel(GPDMA1, LL_DMA_CHANNEL_0);
  LL_TIM_EnableIT_UPDATE(TIM6);
  NVIC_EnableIRQ(TIM6_IRQn);
  LL_TIM_EnableDMAReq_UPDATE(TIM6);
  LL_TIM_EnableCounter(TIM6);
  while(dma_done == 0)
  {
    __WFI();
  }
  LOG(updates);
  DONE();
}

void tim6_poll(void)
{
  LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_TIM6);
  time_base(TIM6, 3999, 49);
  LL_TIM_EnableCounter(TIM6);
  for(uint32_t seen = 0; seen < 2; ++seen)
  {
    while(!LL_TIM_IsActiveFlag_UPDATE(TIM6))
    {
    }
    LL_TIM_ClearFlag_UPDATE(TIM6);
  }
  LOG(2);
  DONE();
}

static void poll_updates(TIM_TypeDef* tim, uint32_t count)
{
  for(uint32_t seen = 0; seen < count; ++seen)
  {
    while(!LL_TIM_IsActiveFlag_UPDATE(tim))
    {
    }
    LL_TIM_ClearFlag_UPDATE(tim);
  }
}

void tim4_triggers_tim5(void)
{
  LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_TIM4 | LL_APB1_GRP1_PERIPH_TIM5);
  time_base(TIM4, 3, 99);
  LL_TIM_SetTriggerOutput(TIM4, LL_TIM_TRGO_UPDATE);
  time_base(TIM5, 3, 0xffffffffu);
  LL_TIM_SetTriggerInput(TIM5, LL_TIM_TS_ITR3);
  LL_TIM_SetSlaveMode(TIM5, LL_TIM_SLAVEMODE_TRIGGER);
  LL_TIM_ClearFlag_TRIG(TIM5);
  LL_TIM_EnableCounter(TIM4);
  poll_updates(TIM4, 10);
  LOG(LL_TIM_IsEnabledCounter(TIM5));
  LOG(LL_TIM_IsActiveFlag_TRIG(TIM5));
  LOG(LL_TIM_GetCounter(TIM5));
  DONE();
}

void tim1_clocks_tim15(void)
{
  clock_160mhz();
  LL_APB2_GRP1_EnableClock(LL_APB2_GRP1_PERIPH_TIM1 | LL_APB2_GRP1_PERIPH_TIM15);
  time_base(TIM1, 159, 999);
  LL_TIM_SetTriggerOutput(TIM1, LL_TIM_TRGO_UPDATE);
  LL_TIM_EnableIT_UPDATE(TIM1);
  NVIC_EnableIRQ(TIM1_UP_IRQn);
  time_base(TIM15, 0, 0xffff);
  LL_TIM_SetTriggerInput(TIM15, LL_TIM_TS_ITR0);
  LL_TIM_SetClockSource(TIM15, LL_TIM_CLOCKSOURCE_EXT_MODE1);
  LL_TIM_EnableCounter(TIM15);
  LL_TIM_EnableCounter(TIM1);
  while(updates < 20)
  {
    __WFI();
  }
  LOG(LL_TIM_GetCounter(TIM15));
  DONE();
}

void tim15_unclocked(void)
{
  LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_TIM6);
  LL_APB2_GRP1_EnableClock(LL_APB2_GRP1_PERIPH_TIM15);
  time_base(TIM6, 3, 999);
  time_base(TIM15, 0, 0xffff);
  LL_TIM_SetTriggerInput(TIM15, LL_TIM_TS_ITR0);
  LL_TIM_SetClockSource(TIM15, LL_TIM_CLOCKSOURCE_EXT_MODE1);
  LL_TIM_EnableCounter(TIM15);
  LL_TIM_EnableCounter(TIM6);
  poll_updates(TIM6, 1);
  LOG(LL_TIM_GetCounter(TIM15));
  DONE();
}

void tim3_encoder_idle(void)
{
  clock_160mhz();
  LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_TIM3 | LL_APB1_GRP1_PERIPH_TIM6);
  time_base(TIM6, 159, 999);
  time_base(TIM3, 0, 0xffff);
  LL_TIM_IC_SetActiveInput(TIM3, LL_TIM_CHANNEL_CH1, LL_TIM_ACTIVEINPUT_DIRECTTI);
  LL_TIM_IC_SetActiveInput(TIM3, LL_TIM_CHANNEL_CH2, LL_TIM_ACTIVEINPUT_DIRECTTI);
  LL_TIM_SetEncoderMode(TIM3, LL_TIM_ENCODERMODE_X4_TI12);
  LL_TIM_EnableIT_UPDATE(TIM3);
  NVIC_EnableIRQ(TIM3_IRQn);
  LL_TIM_EnableCounter(TIM3);
  LL_TIM_EnableCounter(TIM6);
  poll_updates(TIM6, 5);
  LOG(updates);
  LOG(LL_TIM_GetCounter(TIM3));
  DONE();
}

void tim6_dma_bsrr(void)
{
  LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_TIM6);
  LL_AHB1_GRP1_EnableClock(LL_AHB1_GRP1_PERIPH_GPDMA1);
  LL_AHB2_GRP1_EnableClock(LL_AHB2_GRP1_PERIPH_GPIOE);
  time_base(TIM6, 3, 99);
  word_channel(LL_DMA_CHANNEL_0, LL_GPDMA1_REQUEST_TIM6_UP, pattern, 8, (uint32_t)&GPIOE->BSRR);
  NVIC_EnableIRQ(GPDMA1_Channel0_IRQn);
  LL_DMA_EnableChannel(GPDMA1, LL_DMA_CHANNEL_0);
  LL_TIM_EnableIT_UPDATE(TIM6);
  NVIC_EnableIRQ(TIM6_IRQn);
  LL_TIM_EnableDMAReq_UPDATE(TIM6);
  LL_TIM_EnableCounter(TIM6);
  while(dma_done == 0)
  {
    __WFI();
  }
  DONE();
}

void tim2_two_requests(void)
{
  LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_TIM2 | LL_APB1_GRP1_PERIPH_TIM6);
  LL_AHB1_GRP1_EnableClock(LL_AHB1_GRP1_PERIPH_GPDMA1);
  time_base(TIM6, 3, 99);
  time_base(TIM2, 3, 99);
  LL_TIM_OC_SetMode(TIM2, LL_TIM_CHANNEL_CH1, LL_TIM_OCMODE_FROZEN);
  LL_TIM_OC_SetCompareCH1(TIM2, 0);
  word_channel(LL_DMA_CHANNEL_1, LL_GPDMA1_REQUEST_TIM2_UP, thirds, 4, (uint32_t)&TIM2->CCR3);
  word_channel(LL_DMA_CHANNEL_2, LL_GPDMA1_REQUEST_TIM2_CH1, fourths, 4, (uint32_t)&TIM2->CCR4);
  NVIC_EnableIRQ(GPDMA1_Channel1_IRQn);
  NVIC_EnableIRQ(GPDMA1_Channel2_IRQn);
  LL_DMA_EnableChannel(GPDMA1, LL_DMA_CHANNEL_1);
  LL_DMA_EnableChannel(GPDMA1, LL_DMA_CHANNEL_2);
  LL_TIM_EnableDMAReq_UPDATE(TIM2);
  LL_TIM_EnableDMAReq_CC1(TIM2);
  LL_TIM_EnableCounter(TIM2);
  LL_TIM_EnableCounter(TIM6);
  uint32_t ticks = 0;
  for(; ticks < 20 && (channel_done[1] == 0 || channel_done[2] == 0); ++ticks)
  {
    poll_updates(TIM6, 1);
  }
  LOG(ticks);
  LOG(channel_done[1]);
  LOG(channel_done[2]);
  LOG(LL_DMA_GetBlkDataLength(GPDMA1, LL_DMA_CHANNEL_1));
  LOG(LL_DMA_GetBlkDataLength(GPDMA1, LL_DMA_CHANNEL_2));
  LOG(LL_TIM_OC_GetCompareCH3(TIM2));
  LOG(LL_TIM_OC_GetCompareCH4(TIM2));
  DONE();
}

static void log_tim5_triple(void)
{
  LOG(LL_TIM_OC_GetCompareCH1(TIM5));
  LOG(LL_TIM_OC_GetCompareCH2(TIM5));
  LOG(LL_TIM_OC_GetCompareCH3(TIM5));
}

void tim5_burst_cpu(void)
{
  LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_TIM5);
  LL_TIM_ConfigDMABurst(
      TIM5, LL_TIM_DMABURST_BASEADDR_CCR1, LL_TIM_DMABURST_LENGTH_3TRANSFERS, LL_TIM_DMA_UPDATE);
  TIM5->DMAR = 11;
  TIM5->DMAR = 22;
  TIM5->DMAR = 33;
  log_tim5_triple();
  DONE();
}

void tim5_burst_dma(void)
{
  LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_TIM5 | LL_APB1_GRP1_PERIPH_TIM6);
  LL_AHB1_GRP1_EnableClock(LL_AHB1_GRP1_PERIPH_GPDMA1);
  time_base(TIM5, 3, 99);
  time_base(TIM6, 3, 99);
  LL_TIM_ConfigDMABurst(
      TIM5, LL_TIM_DMABURST_BASEADDR_CCR1, LL_TIM_DMABURST_LENGTH_3TRANSFERS, LL_TIM_DMA_UPDATE);
  word_channel(LL_DMA_CHANNEL_0, LL_GPDMA1_REQUEST_TIM5_UP, nine, 9, (uint32_t)&TIM5->DMAR);
  LL_DMA_EnableChannel(GPDMA1, LL_DMA_CHANNEL_0);
  LL_TIM_EnableDMAReq_UPDATE(TIM5);
  LL_TIM_EnableCounter(TIM5);
  while(LL_TIM_GetCounter(TIM5) < 50)
  {
  }
  LL_TIM_EnableCounter(TIM6);
  for(uint32_t update = 0; update < 3; ++update)
  {
    poll_updates(TIM6, 1);
    log_tim5_triple();
    LOG(LL_DMA_GetBlkDataLength(GPDMA1, LL_DMA_CHANNEL_0));
  }
  LOG(LL_DMA_IsEnabledChannel(GPDMA1, LL_DMA_CHANNEL_0));
  DONE();
}

int main(void)
{
  tim16_updates();
}
