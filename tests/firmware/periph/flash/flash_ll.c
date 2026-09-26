#include "periph_test.h"
#include "stm32u5xx_ll_system.h"

#define NSKEY1 0x45670123u
#define NSKEY2 0xcdef89abu

static uint32_t locked(void)
{
  return READ_BIT(FLASH->NSCR, FLASH_NSCR_LOCK) != 0 ? 1u : 0u;
}

void wrong_key(void);
void HardFault_Handler(void);

void HardFault_Handler(void)
{
  LOG(0xfa17u);
  FLASH->NSKEYR = NSKEY1;
  FLASH->NSKEYR = NSKEY2;
  LOG(locked());
  DONE();
}

void wrong_key(void)
{
  FLASH->NSKEYR = NSKEY1;
  FLASH->NSKEYR = 0x12345678u;
  for(;;)
  {
  }
}

int main(void)
{
  LL_FLASH_SetLatency(LL_FLASH_LATENCY_4);
  LOG(LL_FLASH_GetLatency());
  LOG(locked());
  FLASH->NSKEYR = NSKEY1;
  FLASH->NSKEYR = NSKEY2;
  LOG(locked());
  SET_BIT(FLASH->NSCR, FLASH_NSCR_LOCK);
  LOG(locked());
  DONE();
}
