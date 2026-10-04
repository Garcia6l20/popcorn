#include "periph_test.h"
#include "stm32u5xx.h"
#include "stm32u5xx_ll_bus.h"
#include "stm32u5xx_ll_pwr.h"
#include "stm32u5xx_ll_rcc.h"

#define SCAN_RANKS       10u
#define TS_CAL1_ADDRESS  0x0BFA0710u
#define TS_CAL2_ADDRESS  0x0BFA0742u
#define VREFINT_ADDRESS  0x0BFA07A5u
#define ADC_RES_12       (1u << ADC_CFGR1_RES_Pos)
#define ADC_EXTSEL_TIM15 (14u << ADC_CFGR1_EXTSEL_Pos)
#define ADC_DMNGT_LOOP   (3u << ADC_CFGR1_DMNGT_Pos)
#define ADC_OVERSAMPLE   ((7u << 16) | (3u << 5) | ADC_CFGR2_ROVSE)
#define SMP_36           4u
#define SMP_68           5u
#define SMP_814          7u
#define AWD3_IN3         0x8u
#define UNPLUGGED_LOW    0u
#define UNPLUGGED_HIGH   7440u
#define PLUGGED_LOW      6944u
#define PLUGGED_HIGH     16384u
#define SCAN_CTR1        0x400A0002u
#define SCAN_CTR2        0x80000000u
#define SCAN_CBR1        40u
#define SCAN_NODE_LINK   0x38010000u
#define SCAN_LIST_LINK   0xF8010000u
#define SCAN_CCR         0x00021F00u
#define TIM15_PSC        4999u
#define TIM15_ARR        15999u
#define TIM2_PSC_1MHZ    159u
#define TAG_START        0x5A000000u
#define TAG_AWD3         0xA3000000u
#define TAG_EOS          0xE5000000u
#define TAG_EVENT        0xCB000000u
#define TAG_OVERRUN      0x0E000000u
#define TAG_HALF         0xD1000000u
#define TAG_DONE         0xDC000000u
#define TAG_DMA_ERROR    0xDE000000u
#define STAMP_MASK       0x00FFFFFFu

struct scan_head
{
  uint32_t ctr1;
  uint32_t ctr2;
  uint32_t cbr1;
  uint32_t csar;
  uint32_t cdar;
  uint32_t cllr;
};

struct scan_tail
{
  uint32_t cbr1;
  uint32_t csar;
  uint32_t cdar;
  uint32_t cllr;
};

static struct scan_head head __attribute__((aligned(32)));
static struct scan_tail tail __attribute__((aligned(32)));

static uint32_t const ranks[SCAN_RANKS] = {2u, 3u, 4u, 5u, 6u, 14u, 15u, 16u, 19u, 0u};

uint32_t          results[SCAN_RANKS];
uint32_t          last[SCAN_RANKS];
volatile uint32_t scans;
volatile uint32_t ends;
volatile uint32_t plugged;
volatile uint32_t detected;
volatile uint32_t calibration;

void idle(void);
void scan_awd(void);
void scanning(void);
void ADC1_IRQHandler(void);
void GPDMA1_Channel1_IRQHandler(void);
int  main(void);

void idle(void)
{
  for(;;)
  {
    __WFI();
  }
}

void __attribute__((noinline)) scanning(void)
{
  __asm__ volatile("" ::: "memory");
}

static uint32_t stamp(uint32_t tag)
{
  return tag | (TIM2->CNT & STAMP_MASK);
}

static void clocks(void)
{
  LL_AHB3_GRP1_EnableClock(LL_AHB3_GRP1_PERIPH_PWR);
  LL_PWR_SetRegulVoltageScaling(LL_PWR_REGU_VOLTAGE_SCALE1);
  while(!LL_PWR_IsActiveFlag_VOS())
  {
  }
  LL_PWR_EnableEPODBooster();
  while(!LL_PWR_IsActiveFlag_BOOST())
  {
  }
  FLASH->ACR = (FLASH->ACR & ~FLASH_ACR_LATENCY) | 4u;
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
  LL_RCC_PLL2_ConfigDomain_ADC(LL_RCC_PLL2SOURCE_MSIS, 1, 36, 2);
  LL_RCC_PLL2_SetVCOInputRange(LL_RCC_PLLINPUTRANGE_4_8);
  LL_RCC_PLL2_EnableDomain_ADC();
  LL_RCC_PLL2_Enable();
  while(!LL_RCC_PLL2_IsReady())
  {
  }
  LL_RCC_SetADCDACClockSource(LL_RCC_ADCDAC_CLKSOURCE_PLL2);
}

static void time_log(void)
{
  LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_TIM2);
  TIM2->PSC = TIM2_PSC_1MHZ;
  TIM2->ARR = 0xFFFFFFFFu;
  TIM2->EGR = TIM_EGR_UG;
  TIM2->SR  = 0u;
  TIM2->CR1 = TIM_CR1_CEN;
}

static void stop(void)
{
  if((ADC1->CR & ADC_CR_ADSTART) != 0)
  {
    ADC1->CR |= ADC_CR_ADSTP;
    while((ADC1->CR & ADC_CR_ADSTP) != 0)
    {
    }
  }
}

static void enable(void)
{
  ADC1->ISR = ADC_ISR_ADRDY;
  ADC1->CR |= ADC_CR_ADEN;
  while((ADC1->ISR & ADC_ISR_ADRDY) == 0)
  {
  }
}

static void disable(void)
{
  ADC1->CR |= ADC_CR_ADDIS;
  while((ADC1->CR & ADC_CR_ADEN) != 0)
  {
  }
}

static void factory_words(void)
{
  LOG(*(uint16_t const volatile*)TS_CAL1_ADDRESS);
  LOG(*(uint16_t const volatile*)TS_CAL2_ADDRESS);
  LOG(*(uint16_t const volatile*)VREFINT_ADDRESS);
}

static void init(void)
{
  PWR->SVMCR |= PWR_SVMCR_ASV;
  LL_AHB2_GRP1_EnableClock(LL_AHB2_GRP1_PERIPH_ADC12);
  LL_AHB2_GRP1_ForceReset(LL_AHB2_GRP1_PERIPH_ADC12);
  LL_AHB2_GRP1_ReleaseReset(LL_AHB2_GRP1_PERIPH_ADC12);
  ADC12_COMMON->CCR = ADC_CCR_VREFEN | ADC_CCR_VSENSEEN;
  ADC1->CR          = 0u;
  ADC1->CR          = ADC_CR_ADVREGEN;
  while((ADC1->ISR & ADC_ISR_LDORDY) == 0)
  {
  }
  ADC1->CFGR1 = ADC_RES_12;
  ADC1->CFGR2 = ADC_OVERSAMPLE;
  ADC1->CR |= ADC_CR_ADCALLIN | ADC_CR_ADCAL;
  while((ADC1->CR & ADC_CR_ADCAL) != 0)
  {
  }
  calibration = ADC1->CALFACT;
  ADC1->CFGR1 =
      ADC_RES_12 | ADC_DMNGT_LOOP | ADC_EXTSEL_TIM15 | ADC_CFGR1_EXTEN_0 | ADC_CFGR1_OVRMOD;
  ADC1->ISR = ADC_ISR_OVR | ADC_ISR_EOS;
  ADC1->IER = ADC_IER_OVRIE;
  enable();
  factory_words();
}

static void set_sampling(uint32_t channel, uint32_t code)
{
  volatile uint32_t* const smpr  = channel < 10u ? &ADC1->SMPR1 : &ADC1->SMPR2;
  uint32_t const           shift = 3u * (channel % 10u);
  *smpr                          = (*smpr & ~(7u << shift)) | (code << shift);
}

static void set_rank(uint32_t rank, uint32_t channel)
{
  uint32_t const           position = rank + 1u;
  volatile uint32_t* const sqr      = &ADC1->SQR1 + position / 5u;
  uint32_t const           shift    = 6u * (position % 5u);
  *sqr                              = (*sqr & ~(0x1Fu << shift)) | (channel << shift);
}

static void sequence(void)
{
  for(uint32_t rank = 0; rank < SCAN_RANKS; ++rank)
  {
    uint32_t const channel = ranks[rank];
    stop();
    disable();
    set_rank(rank, channel);
    set_sampling(
        channel, channel == 5u                       ? SMP_68
                 : (channel == 19u || channel == 0u) ? SMP_814
                                                     : SMP_36);
    ADC1->PCSEL |= 1u << channel;
    ADC1->SQR1 = (ADC1->SQR1 & ~ADC_SQR1_L) | (rank << ADC_SQR1_L_Pos);
    enable();
  }
}

static void scan_list(void)
{
  LL_AHB1_GRP1_EnableClock(LL_AHB1_GRP1_PERIPH_GPDMA1);
  uint32_t const source  = (uint32_t)&ADC1->DR;
  head.ctr1              = SCAN_CTR1;
  head.ctr2              = SCAN_CTR2;
  head.cbr1              = SCAN_CBR1;
  head.csar              = source;
  head.cdar              = (uint32_t)results;
  head.cllr              = SCAN_NODE_LINK | ((uint32_t)&tail & 0xFFFCu);
  tail.cbr1              = SCAN_CBR1;
  tail.csar              = source;
  tail.cdar              = (uint32_t)results;
  tail.cllr              = SCAN_NODE_LINK | ((uint32_t)&tail & 0xFFFCu);
  GPDMA1_Channel1->CTR2  = DMA_CTR2_TCEM_0;
  GPDMA1_Channel1->CBR1  = 0u;
  GPDMA1_Channel1->CLBAR = (uint32_t)&head & 0xFFFF0000u;
  GPDMA1_Channel1->CLLR  = SCAN_LIST_LINK | ((uint32_t)&head & 0xFFFCu);
  GPDMA1_Channel1->CCR   = SCAN_CCR;
  GPDMA1_Channel1->CCR |= DMA_CCR_EN;
  NVIC_EnableIRQ(GPDMA1_Channel1_IRQn);
}

static void thresholds(void)
{
  ADC1->LTR3 = plugged != 0 ? PLUGGED_LOW : UNPLUGGED_LOW;
  ADC1->HTR3 = plugged != 0 ? PLUGGED_HIGH : UNPLUGGED_HIGH;
}

static void watchdog(void)
{
  ADC1->AWD3CR = AWD3_IN3;
  thresholds();
  ADC1->ISR = ADC_ISR_AWD3;
  ADC1->IER |= ADC_IER_AWD3IE | ADC_IER_EOSIE;
  NVIC_EnableIRQ(ADC1_IRQn);
}

static void trigger(void)
{
  LL_APB2_GRP1_EnableClock(LL_APB2_GRP1_PERIPH_TIM15);
  TIM15->PSC = TIM15_PSC;
  TIM15->ARR = TIM15_ARR;
  TIM15->EGR = TIM_EGR_UG;
  TIM15->SR  = 0u;
  TIM15->CR2 = (TIM15->CR2 & ~TIM_CR2_MMS) | TIM_CR2_MMS_1;
  TIM15->CR1 = TIM_CR1_CEN;
  scanning();
  LOG(stamp(TAG_START));
}

void scan_awd(void)
{
  clocks();
  time_log();
  init();
  sequence();
  scan_list();
  watchdog();
  trigger();
  ADC1->CR |= ADC_CR_ADSTART;
  idle();
}

static void plug_event(void)
{
  plugged = plugged != 0 ? 0u : 1u;
  LOG(TAG_EVENT | (plugged << 16) | (ends & 0xFFFFu));
  stop();
  thresholds();
  ADC1->ISR = ADC_ISR_AWD3;
  ADC1->CR |= ADC_CR_ADSTART;
}

void ADC1_IRQHandler(void)
{
  uint32_t const now = stamp(0u);
  uint32_t const isr = ADC1->ISR;
  if((isr & ADC_ISR_EOS) != 0 && (ADC1->IER & ADC_IER_EOSIE) != 0)
  {
    ADC1->ISR = ADC_ISR_EOS;
    ++ends;
    LOG(TAG_EOS | now);
    if(detected != 0)
    {
      detected = 0;
      plug_event();
    }
  }
  if((isr & ADC_ISR_OVR) != 0 && (ADC1->IER & ADC_IER_OVRIE) != 0)
  {
    ADC1->ISR = ADC_ISR_OVR;
    LOG(TAG_OVERRUN | (ends & 0xFFFFu));
  }
  if((isr & ADC_ISR_AWD3) != 0 && (ADC1->IER & ADC_IER_AWD3IE) != 0)
  {
    ADC1->ISR = ADC_ISR_AWD3;
    detected  = 1;
    LOG(TAG_AWD3 | now);
  }
}

void GPDMA1_Channel1_IRQHandler(void)
{
  uint32_t const now   = stamp(0u);
  uint32_t const flags = GPDMA1_Channel1->CSR;
  if((flags & DMA_CSR_HTF) != 0)
  {
    GPDMA1_Channel1->CFCR = DMA_CFCR_HTF;
    LOG(TAG_HALF | now);
  }
  if((flags & DMA_CSR_TCF) != 0)
  {
    GPDMA1_Channel1->CFCR = DMA_CFCR_TCF;
    ++scans;
    for(uint32_t rank = 0; rank < SCAN_RANKS; ++rank)
    {
      last[rank] = results[rank];
    }
    LOG(TAG_DONE | now);
  }
  uint32_t const errors = flags & (DMA_CSR_DTEF | DMA_CSR_ULEF | DMA_CSR_USEF);
  if(errors != 0)
  {
    GPDMA1_Channel1->CFCR = errors;
    LOG(TAG_DMA_ERROR | errors);
  }
}

int main(void)
{
  idle();
}
