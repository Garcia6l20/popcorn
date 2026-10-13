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

uint32_t words[32];

void copy_words(uint32_t volatile* dst, uint32_t const* src, uint32_t count);
void store_half(uint16_t volatile* dst, uint32_t value);
void store_byte(uint8_t volatile* dst, uint32_t value);
void store_double(uint32_t volatile* dst, uint32_t low, uint32_t high);
void wait_idle(void);

__attribute__((noipa)) void copy_words(uint32_t volatile* dst, uint32_t const* src, uint32_t count)
{
  for(uint32_t i = 0; i < count; ++i)
  {
    dst[i] = src[i];
  }
  DONE();
}

__attribute__((noipa)) void store_half(uint16_t volatile* dst, uint32_t value)
{
  *dst = (uint16_t)value;
  DONE();
}

__attribute__((noipa)) void store_byte(uint8_t volatile* dst, uint32_t value)
{
  *dst = (uint8_t)value;
  DONE();
}

__attribute__((noipa)) void store_double(uint32_t volatile* dst, uint32_t low, uint32_t high)
{
  __asm__ volatile("vmov d0, %1, %2\nvstr d0, [%0]" ::"r"(dst), "r"(low), "r"(high)
                   : "d0", "memory");
  DONE();
}

__attribute__((noipa)) void wait_idle(void)
{
  while(READ_BIT(FLASH->NSSR, FLASH_NSSR_BSY) != 0)
  {
  }
  DONE();
}

void spin(uint32_t count);

__attribute__((noipa)) void spin(uint32_t count)
{
  for(uint32_t volatile i = 0; i < count; ++i)
  {
  }
  DONE();
}

void burst_program(uint32_t volatile* dst, uint32_t const* src, uint32_t clear_first);

__attribute__((noipa)) void
    burst_program(uint32_t volatile* dst, uint32_t const* src, uint32_t clear_first)
{
  SET_BIT(FLASH->NSCR, FLASH_NSCR_PG | FLASH_NSCR_BWR);
  for(uint32_t i = 0; i < 32u; ++i)
  {
    dst[i] = src[i];
  }
  if(clear_first != 0u)
  {
    CLEAR_BIT(FLASH->NSCR, FLASH_NSCR_PG | FLASH_NSCR_BWR);
  }
  while(READ_BIT(FLASH->NSSR, FLASH_NSSR_BSY) != 0)
  {
  }
  CLEAR_BIT(FLASH->NSCR, FLASH_NSCR_PG | FLASH_NSCR_BWR);
  DONE();
}
