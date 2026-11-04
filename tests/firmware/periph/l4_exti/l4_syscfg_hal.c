#include "periph_test.h"
#include "stm32l4xx_hal.h"

#define SRAM2_FIRST (*(volatile uint32_t*)0x10000000u)
#define SRAM2_LAST  (*(volatile uint32_t*)0x10007FFCu)

static volatile uint32_t sram1_marker;

void idle(void);

void idle(void)
{
  for(;;)
  {
    __WFI();
  }
}

int main(void)
{
  __HAL_RCC_SYSCFG_CLK_ENABLE();
  SRAM2_FIRST  = 0x5a5a0001u;
  SRAM2_LAST   = 0x5a5a0002u;
  sram1_marker = 0xa5a50003u;
  HAL_SYSCFG_SRAM2Erase();
  LOG(SYSCFG->SCSR);
  LOG(SRAM2_FIRST);
  uint32_t polls = 0;
  while((SYSCFG->SCSR & SYSCFG_SCSR_SRAM2BSY) != 0u)
  {
    ++polls;
  }
  LOG(polls != 0u);
  LOG(SYSCFG->SCSR);
  LOG(SRAM2_FIRST);
  LOG(SRAM2_LAST);
  LOG(sram1_marker);
  DONE();
}
