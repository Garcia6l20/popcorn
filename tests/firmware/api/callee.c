#include <stdint.h>

extern uint32_t _estack;
extern uint32_t _sidata[];
extern uint32_t _sdata[];
extern uint32_t _edata[];
extern uint32_t _sbss[];
extern uint32_t _ebss[];

#define SYST_CSR (*(volatile uint32_t*)0xE000E010u)
#define SYST_RVR (*(volatile uint32_t*)0xE000E014u)
#define SYST_CVR (*(volatile uint32_t*)0xE000E018u)

volatile uint32_t ticks;
volatile uint32_t loops;
volatile uint32_t flag;

struct point
{
  int32_t x;
  int32_t y;
  float   scale;
};

__attribute__((noipa)) int32_t add(int32_t a, int32_t b)
{
  return a + b;
}

__attribute__((noipa)) int32_t negate(int32_t a)
{
  return -a;
}

__attribute__((noipa)) uint32_t ones(void)
{
  return 0xFFFFFFFFu;
}

__attribute__((noipa)) int64_t twice64(int64_t v)
{
  return v * 2;
}

__attribute__((noipa)) float mulf(float a, float b)
{
  return a * b;
}

__attribute__((noipa)) double half(double d)
{
  return d / 2.0;
}

__attribute__((noipa)) int32_t
    sum6(int32_t a, int32_t b, int32_t c, int32_t d, int32_t e, int32_t f)
{
  return a + 2 * b + 3 * c + 4 * d + 5 * e + 6 * f;
}

__attribute__((noipa)) void fill(struct point* p, int32_t x)
{
  p->x     = x;
  p->y     = -x;
  p->scale = 1.5f;
}

__attribute__((noipa)) uint8_t low_byte(uint8_t v)
{
  return v;
}

__attribute__((noipa)) int16_t neg16(int16_t v)
{
  return (int16_t)-v;
}

__attribute__((noipa, pcs("aapcs"))) double
    soft_mixed(int32_t a, double b, int32_t c, float d, int64_t e)
{
  return (double)a + b + (double)c + (double)d + (double)e;
}

__attribute__((noipa)) int32_t by_value(struct point p)
{
  return p.x;
}

__attribute__((noipa)) double mixed(int32_t a, double b, int32_t c, float d, int64_t e)
{
  return (double)a + b + (double)c + (double)d + (double)e;
}

__attribute__((noipa)) uint32_t bump(void)
{
  static uint32_t counter;
  counter = counter + 1u;
  return counter;
}

__attribute__((naked, noipa)) uint32_t stack_now(void)
{
  __asm__ volatile("mov r0, sp\n"
                   "bx lr\n");
}

__attribute__((naked, noipa)) uint32_t control_now(void)
{
  __asm__ volatile("mrs r0, control\n"
                   "bx lr\n");
}

__attribute__((noipa)) void forever(void)
{
  for(;;)
  {
    loops = loops + 1u;
  }
}

__attribute__((noipa)) void fault(void)
{
  *(volatile uint32_t*)0x60000000u = 1u;
}

__attribute__((noipa)) uint32_t wait_flag(void)
{
  flag = 0;
  while(flag == 0u)
  {
  }
  return ticks;
}

__attribute__((noipa)) int32_t apply(int32_t (*fn)(int32_t), int32_t x)
{
  return fn(x);
}

__attribute__((noipa, optimize("no-optimize-sibling-calls"))) int32_t negated(int32_t a)
{
  return negate(a);
}

__attribute__((noipa)) float scaled(float a)
{
  return mulf(a, 2.0f) + 1.0f;
}

__attribute__((noipa)) double halved(double d)
{
  return half(d) + 1.0;
}

__attribute__((noipa)) void touch(void)
{
  flag = 2u;
}

__attribute__((noipa)) int32_t touched(void)
{
  touch();
  return 1;
}

__attribute__((noipa, optimize("no-optimize-sibling-calls"))) int32_t depth(int32_t n)
{
  return n == 0 ? 0 : depth(n - 1) + 1;
}

struct pair
{
  int32_t a;
  int32_t b;
};

__attribute__((noipa)) struct pair make_pair(int32_t a)
{
  struct pair made = {a, -a};
  return made;
}

__attribute__((noipa, optimize("no-optimize-sibling-calls"))) int32_t use_pair(int32_t a)
{
  return make_pair(a).b;
}

__attribute__((noipa)) uint32_t ticks_now(void)
{
  return ticks;
}

__attribute__((noipa)) uint32_t tick(void)
{
  ticks = ticks + 1u;
  return ticks;
}

void SysTick_Handler(void)
{
  tick();
  flag = 1u;
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
  SYST_RVR = 999u;
  SYST_CVR = 0u;
  SYST_CSR = 7u;
  __asm__ volatile("cpsie i" ::: "memory");
  for(;;)
  {
    loops = loops + 1u;
  }
}

__attribute__((section(".isr_vector"), used)) const void* const vectors[16] = {
    [0]  = &_estack,
    [1]  = (const void*)Reset_Handler,
    [15] = (const void*)SysTick_Handler,
};
