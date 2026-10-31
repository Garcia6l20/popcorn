#include "periph_test.h"
#include "stm32l1xx.h"
#include "stm32l1xx_ll_iwdg.h"
#include "stm32l1xx_ll_rcc.h"

void idle(void);
void refresh(void) __attribute__((noinline));
void updates(void);

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

static void start(uint32_t reload)
{
  LL_IWDG_Enable(IWDG);
  LL_IWDG_EnableWriteAccess(IWDG);
  LL_IWDG_SetPrescaler(IWDG, LL_IWDG_PRESCALER_32);
  LL_IWDG_SetReloadCounter(IWDG, reload);
  while(!LL_IWDG_IsReady(IWDG))
  {
  }
  refresh();
}

void updates(void)
{
  LL_IWDG_Enable(IWDG);
  LL_IWDG_EnableWriteAccess(IWDG);
  LL_IWDG_SetPrescaler(IWDG, LL_IWDG_PRESCALER_256);
  LOG(IWDG->SR);
  LL_IWDG_SetReloadCounter(IWDG, 500);
  LOG(IWDG->SR);
  while(!LL_IWDG_IsReady(IWDG))
  {
  }
  LOG(IWDG->SR);
  LOG(LL_IWDG_GetPrescaler(IWDG));
  LOG(LL_IWDG_GetReloadCounter(IWDG));
  refresh();
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
