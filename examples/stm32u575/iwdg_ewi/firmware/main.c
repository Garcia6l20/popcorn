#include "main.h"

#include "stm32u5xx.h"
#include "stm32u5xx_ll_iwdg.h"

#define RELOAD          2048u
#define EARLY_WAKEUP    204u
#define WORK_ITERATIONS 16u

volatile uint32_t reloads;

__attribute__((noipa)) void watchdog_starting(void)
{}

__attribute__((noipa)) void watchdog_configured(void)
{}

static void work(void)
{
  for(volatile uint32_t index = 0; index < WORK_ITERATIONS; ++index)
  {
  }
}

int main(void)
{
  watchdog_starting();
  LL_IWDG_Enable(IWDG);
  LL_IWDG_EnableWriteAccess(IWDG);
  LL_IWDG_SetPrescaler(IWDG, LL_IWDG_PRESCALER_8);
  LL_IWDG_SetReloadCounter(IWDG, RELOAD);
  WRITE_REG(IWDG->EWCR, IWDG_EWCR_EWIE | EARLY_WAKEUP);
  while(LL_IWDG_IsReady(IWDG) == 0)
  {
  }
  LL_IWDG_ReloadCounter(IWDG);
  watchdog_configured();
  for(;;)
  {
    work();
    LL_IWDG_ReloadCounter(IWDG);
    ++reloads;
  }
}
