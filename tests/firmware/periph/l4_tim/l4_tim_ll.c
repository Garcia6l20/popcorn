#include "periph_test.h"
#include "stm32l4xx.h"
#include "stm32l4xx_ll_bus.h"
#include "stm32l4xx_ll_gpio.h"
#include "stm32l4xx_ll_rcc.h"
#include "stm32l4xx_ll_system.h"
#include "stm32l4xx_ll_tim.h"

#define PERIODS     100u
#define PWM_PERIODS 10u
#define CAPTURES    4u

volatile uint32_t updates;
volatile uint32_t compares;
volatile uint32_t captured[CAPTURES];
volatile uint32_t captures;

void idle(void);
void tim2_update(void);
void tim2_pwm(void);
void tim3_capture(void);
void tim6_trgo(void);
void tim7_update(void);
void on_update(void);
void on_compare(void);
void on_started(void);
void TIM2_IRQHandler(void);
void TIM3_IRQHandler(void);
void TIM6_DAC_IRQHandler(void);
void TIM7_IRQHandler(void);

void idle(void)
{
  for(;;)
  {
    __WFI();
  }
}

__attribute__((noinline)) void on_update(void)
{
  __asm__ volatile("" ::: "memory");
}

__attribute__((noinline)) void on_compare(void)
{
  __asm__ volatile("" ::: "memory");
}

__attribute__((noinline)) void on_started(void)
{
  __asm__ volatile("" ::: "memory");
}

static void hsi_16mhz(void)
{
  LL_RCC_HSI_Enable();
  while(!LL_RCC_HSI_IsReady())
  {
  }
  LL_FLASH_SetLatency(LL_FLASH_LATENCY_1);
  LL_RCC_SetAHBPrescaler(LL_RCC_SYSCLK_DIV_1);
  LL_RCC_SetAPB1Prescaler(LL_RCC_APB1_DIV_1);
  LL_RCC_SetAPB2Prescaler(LL_RCC_APB2_DIV_1);
  LL_RCC_SetSysClkSource(LL_RCC_SYS_CLKSOURCE_HSI);
  while(LL_RCC_GetSysClkSource() != LL_RCC_SYS_CLKSOURCE_STATUS_HSI)
  {
  }
}

static void time_base(TIM_TypeDef* tim, uint32_t prescaler, uint32_t reload)
{
  LL_TIM_SetPrescaler(tim, prescaler);
  LL_TIM_SetAutoReload(tim, reload);
  LL_TIM_GenerateEvent_UPDATE(tim);
  LL_TIM_ClearFlag_UPDATE(tim);
}

static void wait_for(volatile uint32_t const* counter, uint32_t target)
{
  while(*counter < target)
  {
    __WFI();
  }
}

static void count_update(TIM_TypeDef* tim, uint32_t last)
{
  if(LL_TIM_IsActiveFlag_UPDATE(tim))
  {
    LL_TIM_ClearFlag_UPDATE(tim);
    on_update();
    if(++updates == last)
    {
      LL_TIM_DisableCounter(tim);
    }
  }
}

void TIM2_IRQHandler(void)
{
  if(LL_TIM_IsActiveFlag_CC1(TIM2) && LL_TIM_IsEnabledIT_CC1(TIM2))
  {
    LL_TIM_ClearFlag_CC1(TIM2);
    on_compare();
    ++compares;
  }
  count_update(TIM2, PERIODS);
}

void TIM3_IRQHandler(void)
{
  if(LL_TIM_IsActiveFlag_CC1(TIM3))
  {
    uint32_t const value = LL_TIM_IC_GetCaptureCH1(TIM3);
    if(captures < CAPTURES)
    {
      captured[captures] = value;
    }
    ++captures;
  }
}

void TIM6_DAC_IRQHandler(void)
{
  count_update(TIM6, PWM_PERIODS);
}

void TIM7_IRQHandler(void)
{
  count_update(TIM7, PWM_PERIODS);
}

void tim2_update(void)
{
  hsi_16mhz();
  LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_TIM2);
  time_base(TIM2, 15, 999);
  LL_TIM_EnableIT_UPDATE(TIM2);
  NVIC_EnableIRQ(TIM2_IRQn);
  LL_TIM_EnableCounter(TIM2);
  on_started();
  wait_for(&updates, PERIODS);
  LOG(updates);
  DONE();
}

void tim2_pwm(void)
{
  hsi_16mhz();
  LL_AHB2_GRP1_EnableClock(LL_AHB2_GRP1_PERIPH_GPIOA);
  LL_GPIO_SetPinMode(GPIOA, LL_GPIO_PIN_5, LL_GPIO_MODE_ALTERNATE);
  LL_GPIO_SetAFPin_0_7(GPIOA, LL_GPIO_PIN_5, LL_GPIO_AF_1);
  LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_TIM2);
  time_base(TIM2, 15, 999);
  LL_TIM_OC_SetMode(TIM2, LL_TIM_CHANNEL_CH1, LL_TIM_OCMODE_PWM1);
  LL_TIM_OC_SetCompareCH1(TIM2, 250);
  LL_TIM_OC_EnablePreload(TIM2, LL_TIM_CHANNEL_CH1);
  LL_TIM_CC_EnableChannel(TIM2, LL_TIM_CHANNEL_CH1);
  LL_TIM_EnableIT_CC1(TIM2);
  LL_TIM_EnableIT_UPDATE(TIM2);
  NVIC_EnableIRQ(TIM2_IRQn);
  LL_TIM_EnableCounter(TIM2);
  on_started();
  wait_for(&updates, PWM_PERIODS);
  LL_TIM_DisableCounter(TIM2);
  LOG(compares);
  LOG(TIM2->CCMR1);
  DONE();
}

void tim3_capture(void)
{
  hsi_16mhz();
  LL_AHB2_GRP1_EnableClock(LL_AHB2_GRP1_PERIPH_GPIOA);
  LL_GPIO_SetPinMode(GPIOA, LL_GPIO_PIN_6, LL_GPIO_MODE_ALTERNATE);
  LL_GPIO_SetAFPin_0_7(GPIOA, LL_GPIO_PIN_6, LL_GPIO_AF_2);
  LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_TIM3);
  time_base(TIM3, 15, 0xFFFF);
  LL_TIM_IC_SetActiveInput(TIM3, LL_TIM_CHANNEL_CH1, LL_TIM_ACTIVEINPUT_DIRECTTI);
  LL_TIM_IC_SetPolarity(TIM3, LL_TIM_CHANNEL_CH1, LL_TIM_IC_POLARITY_RISING);
  LL_TIM_CC_EnableChannel(TIM3, LL_TIM_CHANNEL_CH1);
  LL_TIM_EnableIT_CC1(TIM3);
  NVIC_EnableIRQ(TIM3_IRQn);
  LL_TIM_EnableCounter(TIM3);
  on_started();
  wait_for(&captures, CAPTURES);
  for(uint32_t index = 0; index < CAPTURES; ++index)
  {
    LOG(captured[index]);
  }
  DONE();
}

void tim6_trgo(void)
{
  hsi_16mhz();
  LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_TIM6);
  time_base(TIM6, 15, 99);
  LL_TIM_SetTriggerOutput(TIM6, LL_TIM_TRGO_UPDATE);
  LL_TIM_EnableIT_UPDATE(TIM6);
  NVIC_EnableIRQ(TIM6_DAC_IRQn);
  LL_TIM_EnableCounter(TIM6);
  on_started();
  wait_for(&updates, PWM_PERIODS);
  LOG(updates);
  LOG(TIM6->CR2);
  DONE();
}

void tim7_update(void)
{
  hsi_16mhz();
  LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_TIM7);
  time_base(TIM7, 15, 999);
  LL_TIM_EnableUIFRemap(TIM7);
  LL_TIM_EnableIT_UPDATE(TIM7);
  NVIC_EnableIRQ(TIM7_IRQn);
  LL_TIM_EnableCounter(TIM7);
  on_started();
  wait_for(&updates, PWM_PERIODS);
  LL_TIM_DisableIT_UPDATE(TIM7);
  LL_TIM_GenerateEvent_UPDATE(TIM7);
  LOG(updates);
  LOG(TIM7->CNT);
  DONE();
}

int main(void)
{
  DONE();
}
