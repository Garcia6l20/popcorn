#include "periph_test.h"
#include "stm32l1xx.h"
#include "stm32l1xx_ll_bus.h"
#include "stm32l1xx_ll_rcc.h"
#include "stm32l1xx_ll_wwdg.h"

void idle(void);
void refresh(void) __attribute__((noinline));
void arm(void);

void idle(void)
{
  for(;;)
  {
    __WFI();
  }
}

void refresh(void)
{
  LL_WWDG_SetCounter(WWDG, 0x7F);
}

static void wait(uint32_t loops)
{
  for(volatile uint32_t left = loops; left != 0; --left)
  {
  }
}

void arm(void)
{
  LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_WWDG);
  LL_WWDG_SetPrescaler(WWDG, LL_WWDG_PRESCALER_8);
  LL_WWDG_SetWindow(WWDG, 0x7F);
  LL_WWDG_SetCounter(WWDG, 0x7F);
  LL_WWDG_Enable(WWDG);
  LOG(WWDG->CR);
  LOG(WWDG->CFR);
}

int main(void)
{
  LOG(LL_RCC_IsActiveFlag_WWDGRST());
  if(LL_RCC_IsActiveFlag_WWDGRST())
  {
    DONE();
  }
  arm();
  for(uint32_t count = 0; count < 50; ++count)
  {
    wait(700);
    refresh();
  }
  idle();
}
