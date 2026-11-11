#include "periph_test.h"
#include "stm32u5xx.h"
#include "stm32u5xx_ll_bus.h"
#include "stm32u5xx_ll_lptim.h"
#include "stm32u5xx_ll_rcc.h"

#define PERIODS 10u
#define COMPARE 49u
#define RELOAD  99u
#define SETTLE  2000u

volatile uint32_t compares;
volatile uint32_t reloads;

void idle(void);
void lptim1_continuous(void);
void lptim1_forever(void);
void lptim4_one_shot(void);
void on_compare(void);
void on_reload(void);
void LPTIM1_IRQHandler(void);
void LPTIM4_IRQHandler(void);

void idle(void)
{
  for(;;)
  {
    __WFI();
  }
}

__attribute__((noinline)) void on_compare(void)
{
  __asm__ volatile("" ::: "memory");
}

__attribute__((noinline)) void on_reload(void)
{
  __asm__ volatile("" ::: "memory");
}

static void wait_flag(uint32_t (*active)(LPTIM_TypeDef const*), LPTIM_TypeDef* timer)
{
  while(!active(timer))
  {
  }
}

static void lptim_setup(LPTIM_TypeDef* timer, IRQn_Type line)
{
  LL_RCC_MSIK_Enable();
  while(!LL_RCC_MSIK_IsReady())
  {
  }
  LL_LPTIM_SetPrescaler(timer, LL_LPTIM_PRESCALER_DIV16);
  LL_LPTIM_Enable(timer);
  WRITE_REG(timer->DIER, LPTIM_DIER_CC1IE | LPTIM_DIER_ARRMIE);
  wait_flag(LL_LPTIM_IsActiveFlag_DIEROK, timer);
  LL_LPTIM_ClearFlag_DIEROK(timer);
  LL_LPTIM_SetAutoReload(timer, RELOAD);
  wait_flag(LL_LPTIM_IsActiveFlag_ARROK, timer);
  LL_LPTIM_ClearFlag_ARROK(timer);
  LL_LPTIM_OC_SetCompareCH1(timer, COMPARE);
  wait_flag(LL_LPTIM_IsActiveFlag_CMP1OK, timer);
  LL_LPTIM_ClearFlag_CMP1OK(timer);
  NVIC_EnableIRQ(line);
}

static void serve(LPTIM_TypeDef* timer)
{
  if(LL_LPTIM_IsActiveFlag_CC1(timer))
  {
    LL_LPTIM_ClearFlag_CC1(timer);
    on_compare();
    ++compares;
  }
  if(LL_LPTIM_IsActiveFlag_ARRM(timer))
  {
    LL_LPTIM_ClearFlag_ARRM(timer);
    on_reload();
    ++reloads;
  }
}

void LPTIM1_IRQHandler(void)
{
  serve(LPTIM1);
}

void LPTIM4_IRQHandler(void)
{
  serve(LPTIM4);
}

static void wait_for(volatile uint32_t const* counter, uint32_t target)
{
  while(*counter < target)
  {
    __WFI();
  }
}

void lptim1_continuous(void)
{
  LL_APB3_GRP1_EnableClock(LL_APB3_GRP1_PERIPH_LPTIM1);
  lptim_setup(LPTIM1, LPTIM1_IRQn);
  LL_LPTIM_StartCounter(LPTIM1, LL_LPTIM_OPERATING_MODE_CONTINUOUS);
  wait_for(&reloads, PERIODS);
  LOG(compares);
  LOG(reloads);
  DONE();
}

void lptim1_forever(void)
{
  LL_APB3_GRP1_EnableClock(LL_APB3_GRP1_PERIPH_LPTIM1);
  lptim_setup(LPTIM1, LPTIM1_IRQn);
  LL_LPTIM_StartCounter(LPTIM1, LL_LPTIM_OPERATING_MODE_CONTINUOUS);
  idle();
}

void lptim4_one_shot(void)
{
  LL_APB3_GRP1_EnableClock(LL_APB3_GRP1_PERIPH_LPTIM4);
  lptim_setup(LPTIM4, LPTIM4_IRQn);
  LL_LPTIM_StartCounter(LPTIM4, LL_LPTIM_OPERATING_MODE_ONESHOT);
  wait_for(&reloads, 1u);
  for(volatile uint32_t spin = 0; spin < SETTLE; ++spin)
  {
  }
  LOG(compares);
  LOG(reloads);
  LOG(LL_LPTIM_GetCounter(LPTIM4));
  DONE();
}

int main(void)
{
  DONE();
}
