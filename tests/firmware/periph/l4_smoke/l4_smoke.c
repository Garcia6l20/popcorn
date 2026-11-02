#include "periph_test.h"
#include "stm32l4xx.h"
#include "stm32l4xx_ll_bus.h"
#include "stm32l4xx_ll_gpio.h"

#define RAM_FUNC __attribute__((section(".RamFunc"), noinline, used))
#define SRAM2    __attribute__((section(".sram2"), used))

uint32_t ram_probe(void);

SRAM2 uint32_t volatile sram2_word;

static float volatile scale  = 1.5f;
static float volatile factor = 2.25f;

RAM_FUNC uint32_t ram_probe(void)
{
  return 0x5a5a0001u;
}

int main(void)
{
  LOG(DBGMCU->IDCODE);
  LOG(*(uint16_t const volatile*)FLASHSIZE_BASE);
  LOG(ram_probe());
  sram2_word = 0x5a5a0002u;
  LOG(sram2_word);
  LOG((uint32_t)(scale * factor * 4.0f));

  LL_AHB2_GRP1_EnableClock(LL_AHB2_GRP1_PERIPH_GPIOA);
  LL_GPIO_SetPinMode(GPIOA, LL_GPIO_PIN_5, LL_GPIO_MODE_OUTPUT);
  LOG(RCC->AHB2ENR);
  LOG(GPIOA->MODER);
  LL_GPIO_WriteOutputPort(GPIOA, LL_GPIO_PIN_5);
  LOG(GPIOA->ODR);
  LL_GPIO_WriteOutputPort(GPIOA, 0);
  LOG(GPIOA->ODR);
  DONE();
}
