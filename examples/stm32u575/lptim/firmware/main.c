#include "stm32u5xx.h"
#include "stm32u5xx_ll_bus.h"
#include "stm32u5xx_ll_gpio.h"
#include "stm32u5xx_ll_lptim.h"
#include "stm32u5xx_ll_rcc.h"

#include <stdint.h>

#define RELOAD  3124u
#define COMPARE 1562u

volatile uint32_t periods;
volatile uint32_t compares;

void LPTIM1_IRQHandler(void);
void lptim_started(void);

void LPTIM1_IRQHandler(void)
{
  if(LL_LPTIM_IsActiveFlag_CC1(LPTIM1))
  {
    LL_LPTIM_ClearFlag_CC1(LPTIM1);
    ++compares;
  }
  if(LL_LPTIM_IsActiveFlag_ARRM(LPTIM1))
  {
    LL_LPTIM_ClearFlag_ARRM(LPTIM1);
    ++periods;
    LL_GPIO_TogglePin(GPIOA, LL_GPIO_PIN_5);
  }
}

__attribute__((noinline)) void lptim_started(void)
{
  __asm__ volatile("" ::: "memory");
}

static void MX_GPIO_Init(void)
{
  LL_AHB2_GRP1_EnableClock(LL_AHB2_GRP1_PERIPH_GPIOA);
  LL_GPIO_ResetOutputPin(GPIOA, LL_GPIO_PIN_5);
  LL_GPIO_SetPinMode(GPIOA, LL_GPIO_PIN_5, LL_GPIO_MODE_OUTPUT);
}

static void MX_LPTIM1_Init(void)
{
  LL_RCC_MSIK_Enable();
  while(!LL_RCC_MSIK_IsReady())
  {
  }
  LL_RCC_SetLPTIMClockSource(LL_RCC_LPTIM1_CLKSOURCE_MSIK);
  LL_APB3_GRP1_EnableClock(LL_APB3_GRP1_PERIPH_LPTIM1);
  LL_LPTIM_SetPrescaler(LPTIM1, LL_LPTIM_PRESCALER_DIV128);
  LL_LPTIM_Enable(LPTIM1);
  LL_LPTIM_EnableIT_CC1(LPTIM1);
  while(!LL_LPTIM_IsActiveFlag_DIEROK(LPTIM1))
  {
  }
  LL_LPTIM_ClearFlag_DIEROK(LPTIM1);
  LL_LPTIM_EnableIT_ARRM(LPTIM1);
  while(!LL_LPTIM_IsActiveFlag_DIEROK(LPTIM1))
  {
  }
  LL_LPTIM_ClearFlag_DIEROK(LPTIM1);
  LL_LPTIM_SetAutoReload(LPTIM1, RELOAD);
  while(!LL_LPTIM_IsActiveFlag_ARROK(LPTIM1))
  {
  }
  LL_LPTIM_ClearFlag_ARROK(LPTIM1);
  LL_LPTIM_OC_SetCompareCH1(LPTIM1, COMPARE);
  while(!LL_LPTIM_IsActiveFlag_CMP1OK(LPTIM1))
  {
  }
  LL_LPTIM_ClearFlag_CMP1OK(LPTIM1);
  NVIC_SetPriority(LPTIM1_IRQn, 2);
  NVIC_EnableIRQ(LPTIM1_IRQn);
  LL_LPTIM_StartCounter(LPTIM1, LL_LPTIM_OPERATING_MODE_CONTINUOUS);
}

int main(void)
{
  MX_GPIO_Init();
  MX_LPTIM1_Init();
  lptim_started();
  for(;;)
  {
    __WFI();
  }
}
