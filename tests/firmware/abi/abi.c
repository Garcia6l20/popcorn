#include "../arch/arch_test.h"

#define NVIC_ISER0  0xE000E100u
#define NVIC_ICER0  0xE000E180u
#define NVIC_ISPR0  0xE000E200u
#define NVIC_IPR(n) (0xE000E400u + (n))
#define SYST_CSR    0xE000E010u
#define SYST_RVR    0xE000E014u
#define SYST_CVR    0xE000E018u
#define MPU_CTRL    0xE000ED94u
#define MPU_RNR     0xE000ED98u
#define MPU_RBAR    0xE000ED9Cu
#define MPU_RLAR    0xE000EDA0u
#define TESTDEV_DAT 0x40010058u

struct point
{
  int x;
  int y;
};

volatile uint32_t                    ticks;
volatile uint32_t                    counter;
volatile uint32_t                    svc_value;
volatile uint32_t                    irq3_count;
uint32_t                             sp_result[2];
double                               mixed_seen;
__attribute__((aligned(8))) uint32_t psp_stack[256];

void SysTick_Handler(void)
{
  ticks = ticks + 1u;
}

void SVC_Handler(void)
{
  svc_value = 0x5C3u;
}

void IRQ3_Handler(void)
{
  REG32(NVIC_ICER0) = 1u << 3;
  irq3_count        = irq3_count + 1u;
  (void)REG32(TESTDEV_DAT);
}

__attribute__((naked)) void IRQ0_Handler(void)
{
  __asm__ volatile("1:\nbkpt #1\nb 1b");
}

int add(int a, int b)
{
  return a + b;
}

long long mul64(long long a, long long b)
{
  return a * b;
}

long long add64_after3(int a, int b, int c, long long d)
{
  return a + b + c + d;
}

float fadd(float a, float b)
{
  return a + b;
}

double dmul(double a, double b)
{
  return a * b;
}

float backfill(float a, double b, float c)
{
  return 100.0f * a + (float)(10.0 * b) + c;
}

int sum10(int a0, int a1, int a2, int a3, int a4, int a5, int a6, int a7, int a8, int a9)
{
  return a0 + 2 * a1 + 3 * a2 + 4 * a3 + 5 * a4 + 6 * a5 + 7 * a6 + 8 * a7 + 9 * a8 + 10 * a9;
}

double
    d9(double x0,
       double x1,
       double x2,
       double x3,
       double x4,
       double x5,
       double x6,
       double x7,
       double x8)
{
  return x0 + x1 + x2 + x3 + x4 + x5 + x6 + x7 + 10.0 * x8;
}

__attribute__((noipa)) double mixed(int a, double b, int c, float d, long long e)
{
  return a + 10.0 * b + 100.0 * c + 1000.0 * d + 10000.0 * (double)e;
}

__attribute__((pcs("aapcs"))) float soft_fadd(float a, float b)
{
  return a + b;
}

__attribute__((pcs("aapcs"))) double soft_dmul(double a, double b)
{
  return a * b;
}

__attribute__((noipa, pcs("aapcs"))) double soft_mixed(int a, double b, int c, float d, long long e)
{
  return a + 10.0 * b + 100.0 * c + 1000.0 * d + 10000.0 * (double)e;
}

int struct_sum(struct point const* p)
{
  return p->x + 1000 * p->y;
}

unsigned read_control(void)
{
  unsigned value;
  __asm__ volatile("mrs %0, control" : "=r"(value));
  return value;
}

unsigned try_cpsid(void)
{
  unsigned value;
  __asm__ volatile("cpsid i\nmrs %0, primask" : "=r"(value)::"memory");
  return value;
}

void set_basepri(unsigned value)
{
  __asm__ volatile("msr basepri, %0" ::"r"(value) : "memory");
}

unsigned read_sp_state(void)
{
  uint32_t msp;
  uint32_t psp;
  __asm__ volatile("mrs %0, msp\nmrs %1, psp" : "=r"(msp), "=r"(psp));
  sp_result[0] = msp;
  sp_result[1] = psp;
  return (read_control() >> 1) & 1u;
}

int svc_in_call(void)
{
  svc_value = 0;
  __asm__ volatile("svc #3" ::: "memory");
  return (int)svc_value;
}

void spin_forever(void)
{
  for(;;)
  {
    __asm__ volatile("" ::: "memory");
  }
}

void reprogram_systick(void)
{
  REG32(SYST_CVR) = 0;
  REG32(SYST_RVR) = 1999;
}

void call_mixed(void)
{
  mixed_seen = mixed(1, 2.0, 3, 4.0f, 5);
  __asm__ volatile("bkpt #1" ::: "memory");
}

void call_soft_mixed(void)
{
  mixed_seen = soft_mixed(1, 2.0, 3, 4.0f, 5);
  __asm__ volatile("bkpt #1" ::: "memory");
}

static void start_systick(void)
{
  REG32(SYST_RVR)   = 3999;
  REG32(SYST_CVR)   = 0;
  REG8(NVIC_IPR(3)) = 0x40;
  REG32(NVIC_ISER0) = 1u << 3;
  REG32(SYST_CSR)   = 7;
}

__attribute__((noipa)) void main_loop(void)
{
  start_systick();
  for(;;)
  {
    counter = counter + 1u;
  }
}

__attribute__((noipa)) void main_wfi(void)
{
  start_systick();
  for(;;)
  {
    __asm__ volatile("wfi" ::: "memory");
  }
}

__attribute__((noipa)) void mpu_setup(void)
{
  REG32(MPU_RNR)  = 0;
  REG32(MPU_RBAR) = 0x200BF000u | (2u << 1) | 1u;
  REG32(MPU_RLAR) = 0x200BFFE0u | 1u;
  REG32(MPU_CTRL) = 1u;
  __asm__ volatile("dsb\nisb\nbkpt #1" ::: "memory");
}

__attribute__((noipa)) void unpriv_loop(void)
{
  __asm__ volatile(
      "msr psp, %0\nmovs r0, #3\nmsr control, r0\nisb\n1:\nbkpt #1\nb 1b" ::"r"(&psp_stack[256])
      : "r0", "memory");
}

__attribute__((noipa)) void handler_pause(void)
{
  REG8(NVIC_IPR(0)) = 0x80;
  REG32(NVIC_ISER0) = 1u;
  REG32(NVIC_ISPR0) = 1u;
  __asm__ volatile("dsb\nisb\n1:\nnop\nb 1b" ::: "memory");
}

__attribute__((noipa)) void poll_stat(void)
{
  start_systick();
  while(REG32(0x40020004u) != 0)
  {
  }
  __asm__ volatile("bkpt #1" ::: "memory");
}
