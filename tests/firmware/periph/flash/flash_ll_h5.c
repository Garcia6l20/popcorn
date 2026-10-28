#include "periph_test.h"
#include "stm32h5xx.h"

#define BOOT_ERRORS (FLASH_SR_WRPERR | FLASH_SR_PGSERR | FLASH_SR_STRBERR | FLASH_SR_INCERR)

void NMI_Handler(void);

void NMI_Handler(void)
{
  LOG(0x4e4du);
  LOG(FLASH->ECCDETR);
  DONE();
}

int main(void)
{
  LOG(READ_BIT(FLASH->NSCR, FLASH_CR_LOCK) != 0);
  DONE();
}

uint32_t words[32];

void copy_words(uint32_t volatile* dst, uint32_t const* src, uint32_t count);
void store_half(uint16_t volatile* dst, uint32_t value);
void store_byte(uint8_t volatile* dst, uint32_t value);
void wait_idle(void);
void spin(uint32_t count);
void boot_write(uint32_t volatile* dst, uint32_t const* src, uint32_t polls);
void read_word(uint32_t const volatile* from);

__attribute__((noipa)) void read_word(uint32_t const volatile* from)
{
  LOG(*from);
  DONE();
}

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

__attribute__((noipa)) void wait_idle(void)
{
  while(READ_BIT(FLASH->NSSR, FLASH_SR_BSY) != 0)
  {
  }
  DONE();
}

__attribute__((noipa)) void spin(uint32_t count)
{
  for(uint32_t volatile i = 0; i < count; ++i)
  {
  }
  DONE();
}

__attribute__((noipa)) void boot_write(uint32_t volatile* dst, uint32_t const* src, uint32_t polls)
{
  SET_BIT(FLASH->NSCR, FLASH_CR_PG);
  for(uint32_t i = 0; i < 32u; ++i)
  {
    dst[i] = src[i];
  }
  CLEAR_BIT(FLASH->NSCR, FLASH_CR_PG);
  while(READ_BIT(FLASH->NSSR, FLASH_SR_BSY) != 0)
  {
  }
  FLASH->NSSR   = FLASH_SR_EOP | BOOT_ERRORS;
  uint32_t busy = 0;
  while(busy < polls && READ_BIT(FLASH->NSSR, FLASH_SR_BSY | FLASH_SR_EOP) != 0)
  {
    ++busy;
  }
  LOG(busy);
  DONE();
}
