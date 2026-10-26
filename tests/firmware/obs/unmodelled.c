#include <stdint.h>

extern uint32_t _estack;

#define REG(address) (*(volatile uint32_t*)(address))

#define TESTDEV_RO      REG(0x40010000u)
#define TESTDEV_RW      REG(0x40010008u)
#define TESTDEV_GAP     REG(0x40010100u)
#define UNMODELLED_DATA REG(0x40020000u)
#define UNMODELLED_STAT REG(0x40020004u)

volatile uint32_t sink;

__attribute__((noipa)) uint32_t poll_unmodelled(uint32_t count)
{
  uint32_t sum = 0;
  for(uint32_t index = 0; index < count; ++index)
  {
    sum += UNMODELLED_DATA;
  }
  return sum;
}

__attribute__((noipa)) void touch_stat(uint32_t value)
{
  UNMODELLED_STAT = value;
  sink            = UNMODELLED_STAT + UNMODELLED_STAT;
}

__attribute__((noipa)) uint32_t read_unimplemented(void)
{
  return TESTDEV_RW;
}

__attribute__((noipa)) uint32_t read_no_register(void)
{
  return TESTDEV_GAP;
}

__attribute__((noipa)) uint32_t read_modelled(void)
{
  return TESTDEV_RO;
}

__attribute__((noipa)) void boot_done(void)
{}

[[noreturn]] void Reset_Handler(void)
{
  sink = TESTDEV_GAP;
  boot_done();
  for(;;)
  {
  }
}

__attribute__((section(".isr_vector"), used)) static const void* const vectors[] = {
    [0] = &_estack,
    [1] = (const void*)Reset_Handler,
};
