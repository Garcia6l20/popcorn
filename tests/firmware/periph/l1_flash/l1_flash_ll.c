#include "periph_test.h"
#include "stm32l1xx.h"
#include "stm32l1xx_ll_system.h"

#define PEKEY1  0x89ABCDEFu
#define PEKEY2  0x02030405u
#define PRGKEY1 0x8C9DAEBFu
#define PRGKEY2 0x13141516u
#define TARGET  ((uint32_t volatile*)0x0801F000u)

void idle(void);
void wrong_key(void);
void HardFault_Handler(void);

void idle(void)
{
  for(;;)
  {
    __WFI();
  }
}

void HardFault_Handler(void)
{
  LOG(0xfa17u);
  LOG(FLASH->PECR);
  DONE();
}

void wrong_key(void)
{
  FLASH->PEKEYR = PEKEY1;
  FLASH->PEKEYR = 0x12345678u;
  for(;;)
  {
  }
}

__attribute__((section(".RamFunc"), noinline)) static void program_from_sram(uint32_t value)
{
  *TARGET = value;
  LOG(READ_BIT(FLASH->SR, FLASH_SR_BSY | FLASH_SR_ENDHV));
  uint32_t polls = 0;
  while(READ_BIT(FLASH->SR, FLASH_SR_BSY) != 0)
  {
    ++polls;
  }
  LOG(polls > 0u ? 1u : 0u);
}

int main(void)
{
  LOG(FLASH->PECR);
  LOG(FLASH->SR);
  FLASH->PEKEYR = PEKEY1;
  FLASH->PEKEYR = PEKEY2;
  LOG(FLASH->PECR);
  FLASH->PRGKEYR = PRGKEY1;
  FLASH->PRGKEYR = PRGKEY2;
  LOG(FLASH->PECR);
  program_from_sram(0x12345678u);
  LOG(FLASH->SR);
  LOG(*TARGET);
  FLASH->SR = FLASH_SR_EOP;
  LOG(FLASH->SR);
  *(uint8_t volatile*)TARGET = 0x5au;
  LOG(FLASH->SR);
  FLASH->SR = FLASH_SR_SIZERR;
  SET_BIT(FLASH->PECR, FLASH_PECR_PELOCK);
  LOG(FLASH->PECR);
  TARGET[1] = 0xa5a5a5a5u;
  LOG(FLASH->SR);
  LOG(TARGET[1]);
  LL_FLASH_Enable64bitAccess();
  LL_FLASH_SetLatency(LL_FLASH_LATENCY_1);
  LL_FLASH_EnablePrefetch();
  LOG(FLASH->ACR);
  DONE();
}

uint32_t words[32];

void copy_words(uint32_t volatile* dst, uint32_t const* src, uint32_t count);
void store_byte(uint8_t volatile* dst, uint32_t value);
void store_half(uint16_t volatile* dst, uint32_t value);
void wait_idle(void);

__attribute__((noipa)) void copy_words(uint32_t volatile* dst, uint32_t const* src, uint32_t count)
{
  for(uint32_t i = 0; i < count; ++i)
  {
    dst[i] = src[i];
  }
  DONE();
}

__attribute__((noipa)) void store_byte(uint8_t volatile* dst, uint32_t value)
{
  *dst = (uint8_t)value;
  DONE();
}

__attribute__((noipa)) void store_half(uint16_t volatile* dst, uint32_t value)
{
  *dst = (uint16_t)value;
  DONE();
}

__attribute__((noipa)) void wait_idle(void)
{
  while(READ_BIT(FLASH->SR, FLASH_SR_BSY) != 0)
  {
  }
  DONE();
}

void store_loop(uint32_t volatile* dst, uint32_t count);

__attribute__((noipa)) void store_loop(uint32_t volatile* dst, uint32_t count)
{
  for(uint32_t i = 0; i < count; ++i)
  {
    dst[i] = i;
  }
  DONE();
}
