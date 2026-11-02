#include "periph_test.h"
#include "stm32l4xx.h"
#include "stm32l4xx_ll_system.h"

#define KEY1 0x45670123u
#define KEY2 0xcdef89abu

static uint32_t locked(void)
{
  return READ_BIT(FLASH->CR, FLASH_CR_LOCK) != 0 ? 1u : 0u;
}

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
  FLASH->KEYR = KEY1;
  FLASH->KEYR = KEY2;
  LOG(locked());
  DONE();
}

void wrong_key(void)
{
  FLASH->KEYR = KEY1;
  FLASH->KEYR = 0x12345678u;
  for(;;)
  {
  }
}

int main(void)
{
  LL_FLASH_SetLatency(LL_FLASH_LATENCY_4);
  LOG(LL_FLASH_GetLatency());
  LOG(locked());
  FLASH->KEYR = KEY1;
  FLASH->KEYR = KEY2;
  LOG(locked());
  SET_BIT(FLASH->CR, FLASH_CR_LOCK);
  LOG(locked());
  LOG(READ_BIT(FLASH->CR, FLASH_CR_OPTLOCK) != 0 ? 1u : 0u);
  DONE();
}

uint32_t       words[32];
uint32_t const pattern[4] = {0x01234567u, 0x89abcdefu, 0x5a5aa5a5u, 0x0f0ff0f0u};

void copy_words(uint32_t volatile* dst, uint32_t const* src, uint32_t count);
void copy_words_ram(uint32_t volatile* dst, uint32_t const* src, uint32_t count);
void store_half(uint16_t volatile* dst, uint32_t value);
void store_byte(uint8_t volatile* dst, uint32_t value);
void wait_idle(void);
void read_word(uint32_t const volatile* from);

__attribute__((noipa)) void copy_words(uint32_t volatile* dst, uint32_t const* src, uint32_t count)
{
  for(uint32_t i = 0; i < count; ++i)
  {
    dst[i] = src[i];
  }
  DONE();
}

__attribute__((noipa, section(".RamFunc"))) void
    copy_words_ram(uint32_t volatile* dst, uint32_t const* src, uint32_t count)
{
  for(uint32_t i = 0; i < count; ++i)
  {
    dst[i] = src[i];
  }
  while(READ_BIT(FLASH->SR, FLASH_SR_BSY) != 0)
  {
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
  while(READ_BIT(FLASH->SR, FLASH_SR_BSY) != 0)
  {
  }
  DONE();
}

__attribute__((noipa)) void read_word(uint32_t const volatile* from)
{
  LOG(*from);
  LOG(READ_BIT(FLASH->SR, FLASH_SR_BSY) != 0 ? 1u : 0u);
  DONE();
}
