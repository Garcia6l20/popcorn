#include "periph_test.h"
#include "stm32l4xx.h"
#include "stm32l4xx_ll_iwdg.h"
#include "stm32l4xx_ll_rcc.h"

void idle(void);
void refresh(void) __attribute__((noinline));
void windowed(void);

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

void windowed(void)
{
  LL_IWDG_Enable(IWDG);
  LL_IWDG_EnableWriteAccess(IWDG);
  LL_IWDG_SetPrescaler(IWDG, LL_IWDG_PRESCALER_32);
  LL_IWDG_SetReloadCounter(IWDG, 1000);
  LOG(IWDG->SR);
  LL_IWDG_SetWindow(IWDG, 500);
  LOG(IWDG->SR);
  while(!LL_IWDG_IsReady(IWDG))
  {
  }
  LOG(IWDG->SR);
  LOG(LL_IWDG_GetWindow(IWDG));
  idle();
}

int main(void)
{
  LOG(LL_RCC_IsActiveFlag_IWDGRST());
  if(LL_RCC_IsActiveFlag_IWDGRST())
  {
    DONE();
  }
  LL_IWDG_Enable(IWDG);
  LL_IWDG_EnableWriteAccess(IWDG);
  LL_IWDG_SetPrescaler(IWDG, LL_IWDG_PRESCALER_32);
  LL_IWDG_SetReloadCounter(IWDG, 1000);
  while(!LL_IWDG_IsReady(IWDG))
  {
  }
  refresh();
  for(uint32_t count = 0; count < 50; ++count)
  {
    wait(700);
    refresh();
  }
  idle();
}
