#include "periph_test.h"
#include "stm32l1xx.h"

#define RAM_FUNC __attribute__((section(".RamFunc"), noinline, used))

uint32_t ram_probe(void);

RAM_FUNC uint32_t ram_probe(void)
{
  return 0x5a5a0001u;
}

int main(void)
{
  LOG(DBGMCU->IDCODE);
  LOG(*(uint16_t const volatile*)FLASHSIZE_BASE);
  LOG(ram_probe());
  DONE();
}
