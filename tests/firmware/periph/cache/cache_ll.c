#include "periph_test.h"
#include "stm32u5xx_ll_dcache.h"
#include "stm32u5xx_ll_icache.h"

#define POLL(condition) \
  do                    \
  {                     \
    uint32_t polls = 0; \
    while(!(condition)) \
    {                   \
      ++polls;          \
    }                   \
    LOG(polls);         \
  } while(0)

static volatile uint32_t icache_interrupts;

void ICACHE_IRQHandler(void);

void ICACHE_IRQHandler(void)
{
  ++icache_interrupts;
  LL_ICACHE_ClearFlag_BSYEND();
}

int main(void)
{
  POLL(!LL_ICACHE_IsActiveFlag_BUSY());
  LL_ICACHE_ClearFlag_BSYEND();
  LL_ICACHE_EnableIT_BSYEND();
  NVIC_EnableIRQ(ICACHE_IRQn);
  LL_ICACHE_Invalidate();
  LOG(LL_ICACHE_IsActiveFlag_BUSY());
  POLL(icache_interrupts != 0);
  LOG(icache_interrupts);
  LL_ICACHE_Enable();

  POLL(!LL_DCACHE_IsActiveFlag_BUSY(DCACHE1));
  LL_DCACHE_Enable(DCACHE1);
  LL_DCACHE_SetStartAddress(DCACHE1, 0x20000000u);
  LL_DCACHE_SetEndAddress(DCACHE1, 0x20000fffu);
  LL_DCACHE_SetCommand(DCACHE1, LL_DCACHE_COMMAND_CLEAN_INVALIDATE_BY_ADDR);
  LL_DCACHE_StartCommand(DCACHE1);
  POLL(LL_DCACHE_IsActiveFlag_CMDEND(DCACHE1));
  LL_DCACHE_ClearFlag_CMDEND(DCACHE1);

  LOG(DBGMCU->IDCODE);
  DONE();
}
