#include <stddef.h>
#include <stdint.h>

extern uint32_t _estack;
extern uint32_t _sidata[];
extern uint32_t _sdata[];
extern uint32_t _edata[];
extern uint32_t _sbss[];
extern uint32_t _ebss[];

volatile uint32_t delay_spins;
volatile uint32_t shared;
volatile uint32_t copies;
volatile uint32_t notes;
int32_t           results[3];

__attribute__((noipa)) void hal_delay(uint32_t n)
{
  for(uint32_t i = 0; i < n; ++i)
  {
    delay_spins = delay_spins + 1u;
  }
}

__attribute__((noipa)) int32_t use_delay(void)
{
  hal_delay(1000u);
  return 7;
}

__attribute__((noipa)) int32_t next_value(void)
{
  return 100;
}

__attribute__((noipa)) void run_script(void)
{
  results[0] = next_value();
  results[1] = next_value();
  results[2] = next_value();
}

__attribute__((noipa)) int32_t triple(int32_t a, int32_t b, int32_t c)
{
  return a * 100 + b * 10 + c;
}

__attribute__((noipa)) int32_t use_triple(void)
{
  return triple(1, 2, 3) + triple(4, 5, 6);
}

__attribute__((noipa)) void writer(uint32_t v)
{
  shared = v;
}

__attribute__((noipa)) void write_three(void)
{
  writer(1u);
  writer(2u);
  writer(3u);
}

void* memcpy(void* dest, void const* src, size_t n)
{
  copies                       = copies + 1u;
  volatile uint8_t*       to   = dest;
  volatile uint8_t const* from = src;
  for(size_t i = 0; i < n; ++i)
  {
    to[i] = from[i];
  }
  return dest;
}

void* memset(void* dest, int c, size_t n)
{
  copies               = copies + 1u;
  volatile uint8_t* to = dest;
  for(size_t i = 0; i < n; ++i)
  {
    to[i] = (uint8_t)c;
  }
  return dest;
}

size_t strlen(char const* text)
{
  copies   = copies + 1u;
  size_t n = 0;
  while(((volatile char const*)text)[n] != 0)
  {
    ++n;
  }
  return n;
}

__attribute__((noipa)) uint32_t use_libc(void)
{
  char buffer[16];
  char copy[16];
  memset(buffer, 'a', 5);
  buffer[5] = 0;
  memcpy(copy, buffer, 6);
  return (uint32_t)strlen(copy);
}

__attribute__((noipa, optimize("O0"))) uint32_t fact(uint32_t n)
{
  return n <= 1u ? 1u : n * fact(n - 1u);
}

__attribute__((noipa)) uint32_t use_fact(void)
{
  return fact(3u);
}

struct pair
{
  int32_t a;
  int32_t b;
};

__attribute__((noipa)) int32_t pair_sum(struct pair p)
{
  return p.a + p.b;
}

__attribute__((noipa)) int32_t use_pair(void)
{
  struct pair const p = {2, 3};
  return pair_sum(p);
}

__attribute__((noipa)) uint32_t use_strlen_ff(void)
{
  static char const text[] = "a\xff\xff"
                             "b";
  return (uint32_t)strlen(text);
}

__attribute__((noipa)) void note(void)
{
  notes = notes + 1u;
}

__attribute__((noipa)) void note_then_fault(void)
{
  note();
  *(volatile uint32_t*)0x60000000u = 1u;
}

[[noreturn]] void Reset_Handler(void)
{
  for(uint32_t *from = _sidata, *to = _sdata; to < _edata;)
  {
    *to++ = *from++;
  }
  for(uint32_t* to = _sbss; to < _ebss;)
  {
    *to++ = 0;
  }
  __asm__ volatile("bkpt #0" ::: "memory");
  for(;;)
  {
  }
}

__attribute__((section(".isr_vector"), used)) const void* const vectors[2] = {
    &_estack,
    (const void*)Reset_Handler,
};
