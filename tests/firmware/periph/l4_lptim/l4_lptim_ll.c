#include "periph_test.h"
#include "stm32l4xx.h"
#include "stm32l4xx_ll_bus.h"
#include "stm32l4xx_ll_lptim.h"
#include "stm32l4xx_ll_rcc.h"
#include "stm32l4xx_ll_system.h"

#define PERIODS 10u
#define COMPARE 15u
#define RELOAD  31u
#define SETTLE  20000u

volatile uint32_t compares;
volatile uint32_t reloads;

void idle(void);
void lptim1_continuous(void);
void lptim1_one_shot(void);
void on_compare(void);
void on_reload(void);
void on_started(void);
void LPTIM1_IRQHandler(void);

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

static void lptim1_on_lsi(void)
{
  hsi_16mhz();
  LL_RCC_LSI_Enable();
  while(!LL_RCC_LSI_IsReady())
  {
  }
  LL_RCC_SetLPTIMClockSource(LL_RCC_LPTIM1_CLKSOURCE_LSI);
  LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_LPTIM1);
  LL_LPTIM_EnableIT_CMPM(LPTIM1);
  LL_LPTIM_EnableIT_ARRM(LPTIM1);
  LL_LPTIM_Enable(LPTIM1);
  LL_LPTIM_SetAutoReload(LPTIM1, RELOAD);
  while(!LL_LPTIM_IsActiveFlag_ARROK(LPTIM1))
  {
  }
  LL_LPTIM_ClearFlag_ARROK(LPTIM1);
  LL_LPTIM_SetCompare(LPTIM1, COMPARE);
  while(!LL_LPTIM_IsActiveFlag_CMPOK(LPTIM1))
  {
  }
  LL_LPTIM_ClearFlag_CMPOK(LPTIM1);
  NVIC_EnableIRQ(LPTIM1_IRQn);
}

static void wait_for(volatile uint32_t const* counter, uint32_t target)
{
  while(*counter < target)
  {
    __WFI();
  }
}

void LPTIM1_IRQHandler(void)
{
  if(LL_LPTIM_IsActiveFlag_CMPM(LPTIM1))
  {
    LL_LPTIM_ClearFlag_CMPM(LPTIM1);
    on_compare();
    ++compares;
  }
  if(LL_LPTIM_IsActiveFlag_ARRM(LPTIM1))
  {
    LL_LPTIM_ClearFlag_ARRM(LPTIM1);
    on_reload();
    ++reloads;
  }
}

void lptim1_continuous(void)
{
  lptim1_on_lsi();
  LL_LPTIM_StartCounter(LPTIM1, LL_LPTIM_OPERATING_MODE_CONTINUOUS);
  on_started();
  wait_for(&reloads, PERIODS);
  LOG(compares);
  LOG(reloads);
  DONE();
}

void lptim1_one_shot(void)
{
  lptim1_on_lsi();
  LL_LPTIM_StartCounter(LPTIM1, LL_LPTIM_OPERATING_MODE_ONESHOT);
  on_started();
  wait_for(&reloads, 1u);
  for(volatile uint32_t spin = 0; spin < SETTLE; ++spin)
  {
  }
  LOG(compares);
  LOG(reloads);
  LOG(LL_LPTIM_GetCounter(LPTIM1));
  DONE();
}

int main(void)
{
  DONE();
}
