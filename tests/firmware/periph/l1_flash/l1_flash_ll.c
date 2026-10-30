#include "periph_test.h"
#include "stm32l1xx.h"
#include "stm32l1xx_hal.h"
#include "stm32l1xx_ll_system.h"

#define PEKEY1  0x89ABCDEFu
#define PEKEY2  0x02030405u
#define PRGKEY1 0x8C9DAEBFu
#define PRGKEY2 0x13141516u
#define TARGET  ((uint32_t volatile*)0x0801F000u)

uint32_t HAL_GetTick(void);

uint32_t HAL_GetTick(void)
{
  return 0u;
}

void idle(void);
void wrong_key(void);
void HardFault_Handler(void);

__attribute__((section(".RamFunc"), noinline)) void idle(void)
{
  for(;;)
  {
    __WFI();
  }
}

__attribute__((section(".RamFunc"), noinline)) void HardFault_Handler(void)
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

uint32_t words[64];

void copy_words(uint32_t volatile* dst, uint32_t const* src, uint32_t count);
void store_byte(uint8_t volatile* dst, uint32_t value);
void store_half(uint16_t volatile* dst, uint32_t value);
void wait_idle(void);

__attribute__((section(".RamFunc"), noipa)) void
    copy_words(uint32_t volatile* dst, uint32_t const* src, uint32_t count)
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

__attribute__((section(".RamFunc"), noipa)) void wait_idle(void)
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

void half_page_hal(uint32_t address, uint32_t* buffer);
void erase_page(uint32_t volatile* page);

__attribute__((noipa)) void half_page_hal(uint32_t address, uint32_t* buffer)
{
  LOG((uint32_t)&HAL_FLASHEx_HalfPageProgram);
  LOG(HAL_FLASHEx_HalfPageProgram(address, buffer));
  LOG(FLASH->PECR);
  LOG(FLASH->SR);
  DONE();
}

__attribute__((noipa)) void erase_page(uint32_t volatile* page)
{
  SET_BIT(FLASH->PECR, FLASH_PECR_ERASE | FLASH_PECR_PROG);
  *page = 0u;
  while(READ_BIT(FLASH->SR, FLASH_SR_BSY) != 0)
  {
  }
  CLEAR_BIT(FLASH->PECR, FLASH_PECR_ERASE | FLASH_PECR_PROG);
  DONE();
}

void load_word(uint32_t const volatile* src);
void read_flash(uint32_t const volatile* src);
void parallel_erase_hal(uint32_t first, uint32_t second);
void parallel_half_hal(uint32_t first, uint32_t second, uint32_t* buffer);

__attribute__((section(".RamFunc"), noipa)) void load_word(uint32_t const volatile* src)
{
  LOG(*src);
  DONE();
}

__attribute__((noipa)) void read_flash(uint32_t const volatile* src)
{
  LOG(*src);
  DONE();
}

__attribute__((noipa)) void parallel_erase_hal(uint32_t first, uint32_t second)
{
#if defined(FLASH_PECR_PARALLBANK)
  LOG(HAL_FLASHEx_EraseParallelPage(first, second));
#else
  (void)first;
  (void)second;
#endif
  LOG(FLASH->PECR);
  LOG(FLASH->SR);
  DONE();
}

__attribute__((noipa)) void parallel_half_hal(uint32_t first, uint32_t second, uint32_t* buffer)
{
#if defined(FLASH_PECR_PARALLBANK)
  LOG(HAL_FLASHEx_ProgramParallelHalfPage(first, buffer, second, buffer + 32));
#else
  (void)first;
  (void)second;
  (void)buffer;
#endif
  LOG(FLASH->PECR);
  LOG(FLASH->SR);
  DONE();
}

void copy_words_flash(uint32_t volatile* dst, uint32_t const* src, uint32_t count);

__attribute__((noipa)) void
    copy_words_flash(uint32_t volatile* dst, uint32_t const* src, uint32_t count)
{
  for(uint32_t i = 0; i < count; ++i)
  {
    dst[i] = src[i];
  }
  DONE();
}

void store_pair(uint32_t volatile* dst, uint32_t first, uint32_t second);

__attribute__((noipa)) void store_pair(uint32_t volatile* dst, uint32_t first, uint32_t second)
{
  dst[0] = first;
  dst[1] = second;
  DONE();
}

void eeprom_hal(uint32_t type, uint32_t address, uint32_t data);

__attribute__((noipa)) void eeprom_hal(uint32_t type, uint32_t address, uint32_t data)
{
  LOG(HAL_FLASHEx_DATAEEPROM_Unlock());
  LOG(HAL_FLASHEx_DATAEEPROM_Program(type, address, data));
  LOG(FLASH->SR);
  DONE();
}
