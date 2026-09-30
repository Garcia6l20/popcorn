#include "stm32u5xx.h"
#include "stm32u5xx_ll_bus.h"
#include "stm32u5xx_ll_gpio.h"
#include "stm32u5xx_ll_tim.h"

#include <stdint.h>

#define TIMER_CLOCK_HZ 1000000u
#define TICK_HZ        1000u
#define PULSE_DELAY_MS 50u

volatile uint32_t ticks;
volatile uint32_t pulses;
volatile uint32_t ticks_at_pulse;

void TIM2_IRQHandler(void);
void TIM16_IRQHandler(void);

void TIM2_IRQHandler(void)
{
  if(LL_TIM_IsActiveFlag_UPDATE(TIM2))
  {
    LL_TIM_ClearFlag_UPDATE(TIM2);
    ++ticks;
    LL_GPIO_TogglePin(GPIOA, LL_GPIO_PIN_5);
  }
}

void TIM16_IRQHandler(void)
{
  if(LL_TIM_IsActiveFlag_UPDATE(TIM16))
  {
    LL_TIM_ClearFlag_UPDATE(TIM16);
    ++pulses;
    ticks_at_pulse = ticks;
  }
}

static void MX_GPIO_Init(void)
{
  LL_AHB2_GRP1_EnableClock(LL_AHB2_GRP1_PERIPH_GPIOA);
  LL_GPIO_ResetOutputPin(GPIOA, LL_GPIO_PIN_5);
  LL_GPIO_SetPinMode(GPIOA, LL_GPIO_PIN_5, LL_GPIO_MODE_OUTPUT);
}

static void MX_TIM2_Init(void)
{
  LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_TIM2);
  LL_TIM_SetPrescaler(TIM2, __LL_TIM_CALC_PSC(SystemCoreClock, TIMER_CLOCK_HZ));
  LL_TIM_SetAutoReload(TIM2, TIMER_CLOCK_HZ / TICK_HZ - 1u);
  LL_TIM_GenerateEvent_UPDATE(TIM2);
  LL_TIM_ClearFlag_UPDATE(TIM2);
  LL_TIM_EnableIT_UPDATE(TIM2);
  NVIC_SetPriority(TIM2_IRQn, 2);
  NVIC_EnableIRQ(TIM2_IRQn);
  LL_TIM_EnableCounter(TIM2);
}

static void MX_TIM16_Init(void)
{
  LL_APB2_GRP1_EnableClock(LL_APB2_GRP1_PERIPH_TIM16);
  LL_TIM_SetPrescaler(TIM16, __LL_TIM_CALC_PSC(SystemCoreClock, TICK_HZ));
  LL_TIM_SetAutoReload(TIM16, PULSE_DELAY_MS - 1u);
  LL_TIM_SetOnePulseMode(TIM16, LL_TIM_ONEPULSEMODE_SINGLE);
  LL_TIM_SetUpdateSource(TIM16, LL_TIM_UPDATESOURCE_COUNTER);
  LL_TIM_GenerateEvent_UPDATE(TIM16);
  LL_TIM_ClearFlag_UPDATE(TIM16);
  LL_TIM_EnableIT_UPDATE(TIM16);
  NVIC_SetPriority(TIM16_IRQn, 3);
  NVIC_EnableIRQ(TIM16_IRQn);
  LL_TIM_EnableCounter(TIM16);
}

int main(void)
{
  MX_GPIO_Init();
  MX_TIM2_Init();
  MX_TIM16_Init();
  for(;;)
  {
    __WFI();
  }
}
