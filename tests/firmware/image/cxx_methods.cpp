#include <stdint.h>

extern "C" uint32_t _estack;

struct counter
{
  uint32_t total;

  uint32_t        add(uint32_t amount, uint32_t scale);
  static uint32_t twice(uint32_t value);
};

__attribute__((noinline)) uint32_t counter::add(uint32_t amount, uint32_t scale)
{
  total += amount * scale;
  return total;
}

__attribute__((noinline)) uint32_t counter::twice(uint32_t value)
{
  return value * 2U;
}

counter cxx_counter;

extern "C" [[noreturn]] void Reset_Handler()
{
  uint32_t step = 1U;
  for(;;)
  {
    step = cxx_counter.add(step, 3U) + counter::twice(step);
    __asm__ volatile("bkpt #0" : : "r"(step) : "memory");
  }
}

__attribute__((section(".isr_vector"), used)) static void const* const vectors[] = {
    &_estack,
    reinterpret_cast<void const*>(Reset_Handler),
};
