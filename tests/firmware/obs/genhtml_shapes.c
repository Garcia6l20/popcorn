#include <stdint.h>

extern uint32_t _estack;

volatile uint32_t sink;
volatile uint32_t faults;

__attribute__((noipa)) void note(uint32_t value)
{
  sink = value;
}

static inline __attribute__((always_inline)) uint32_t helper_after(uint32_t v);

__attribute__((noipa)) uint32_t inline_f(uint32_t v)
{
  return helper_after(v) + 1u;
}

static inline __attribute__((always_inline)) uint32_t helper_after(uint32_t v)
{
  uint32_t r = v * 3u;
  sink       = r;
  return r + 7u;
}

__attribute__((noipa)) uint32_t inline_g(uint32_t v)
{
  return helper_after(v) + 2u;
}

__attribute__((noipa)) uint32_t alias_main(uint32_t v)
{
  sink = v;
  return v + 5u;
}

__attribute__((alias("alias_main"))) uint32_t       alias_one(uint32_t v);
__attribute__((weak, alias("alias_main"))) uint32_t alias_two(uint32_t v);

#define PAIR(name)                                     \
  __attribute__((noipa)) uint32_t name##_a(uint32_t v) \
  {                                                    \
    return v + 1u;                                     \
  }                                                    \
  __attribute__((noipa)) uint32_t name##_b(uint32_t v) \
  {                                                    \
    return v + 2u;                                     \
  }

PAIR(same)

__attribute__((noipa)) uint32_t drive(uint32_t v)
{
  uint32_t r = inline_g(v) + alias_one(v) + same_a(v);
  note(r);
  return r;
}

void HardFault_Handler(void)
{
  faults = faults + 1u;
  for(;;)
  {
  }
}

[[noreturn]] void Reset_Handler(void)
{
  drive(1u);
  __asm__ volatile(".global shapes_done\n"
                   "shapes_done:" ::
                       : "memory");
  for(;;)
  {
  }
}

__attribute__((section(".isr_vector"), used)) static const void* const vectors[] = {
    [0] = &_estack,
    [1] = (const void*)Reset_Handler,
    [3] = (const void*)HardFault_Handler,
};
