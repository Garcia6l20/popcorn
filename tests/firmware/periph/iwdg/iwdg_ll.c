#include "periph_test.h"
#include "stm32u5xx_ll_iwdg.h"
#include "stm32u5xx_ll_rcc.h"

void IWDG_IRQHandler(void);
void idle(void);
void refresh(void) __attribute__((noinline));
void window(void);
void early_wakeup(void);

void idle(void)
{
  for(;;)
  {
    __WFI();
  }
}

void refresh(void)
{
  LL_IWDG_ReloadCounter(IWDG);
}

static void wait(uint32_t loops)
{
  for(volatile uint32_t left = loops; left != 0; --left)
  {
  }
}

static void wait_ready(void)
{
  while(!LL_IWDG_IsReady(IWDG))
  {
  }
}

static void start(uint32_t reload)
{
  LL_IWDG_Enable(IWDG);
  LL_IWDG_EnableWriteAccess(IWDG);
  LL_IWDG_SetPrescaler(IWDG, LL_IWDG_PRESCALER_32);
  LL_IWDG_SetReloadCounter(IWDG, reload);
  wait_ready();
  refresh();
}

void IWDG_IRQHandler(void)
{
  LL_IWDG_ClearFlag_EWIF(IWDG);
  LOG(LL_IWDG_IsActiveFlag_EWIF(IWDG));
  DONE();
}

void window(void)
{
  start(1000);
  LL_IWDG_EnableWriteAccess(IWDG);
  LL_IWDG_SetWindow(IWDG, 500);
  wait_ready();
  refresh();
  DONE();
}

void early_wakeup(void)
{
  start(1000);
  LL_IWDG_EnableWriteAccess(IWDG);
  LL_IWDG_SetEwiTime(IWDG, 900);
  wait_ready();
  LL_IWDG_EnableIT_EWI(IWDG);
  wait_ready();
  refresh();
  NVIC_EnableIRQ(IWDG_IRQn);
  idle();
}

int main(void)
{
  LOG(LL_RCC_IsActiveFlag_IWDGRST());
  if(LL_RCC_IsActiveFlag_IWDGRST())
  {
    DONE();
  }
  start(1000);
  for(uint32_t count = 0; count < 50; ++count)
  {
    wait(700);
    refresh();
  }
  idle();
}
