#include "arch_test.h"

#define NVIC_ISER0     0xE000E100u
#define NVIC_ISPR0     0xE000E200u
#define NVIC_IPR(n)    (0xE000E400u + (n))
#define SCB_ICSR       0xE000ED04u
#define SCB_VTOR       0xE000ED08u
#define SCB_CCR        0xE000ED14u
#define SCB_SHPR(n)    (0xE000ED18u + (n) - 4u)
#define SCB_HFSR       0xE000ED2Cu
#define SCB_CPACR      0xE000ED88u
#define NVIC_STIR      0xE000EF00u
#define TESTDEV_RW     0x40010008u
#define UNMODELLED_STA 0x40020004u

#define VECTORS 48

extern void const* const vectors[24];

void svc_entry(void);
void hardfault_entry(void);

__attribute__((aligned(512))) uint32_t ram_vector[VECTORS];
__attribute__((aligned(8))) uint32_t   svc_psp_stack[128];
uint32_t* const                        svc_psp_top = &svc_psp_stack[128];

__attribute__((aligned(8))) uint32_t stack_a[256];
__attribute__((aligned(8))) uint32_t stack_b[256];
uint32_t                             task_sp[2];
uint32_t                             task_sp_first[2];
uint32_t                             control_seen[2];
uint32_t                             b_fp_seen;
uint32_t                             current_task;

void task_a(void);
void task_b(void);
void switch_svc(void);
void switch_pendsv(void);

static inline uint32_t control(void)
{
  uint32_t value;
  __asm__ volatile("mrs %0, control" : "=r"(value));
  return value;
}

static void pend(unsigned irq)
{
  REG32(NVIC_ISPR0) = 1u << irq;
  __asm__ volatile("dsb\nisb" ::: "memory");
}

void svc_dispatch(uint32_t const* frame, uint32_t exc_return)
{
  uint32_t const number = *(uint16_t const*)(frame[6] - 2u) & 0xFFu;
  if(number == 9)
  {
    (void)REG32(TESTDEV_RW);
    return;
  }
  LOG(0x800 | number);
  if(number == 6 || number == 10)
  {
    LOG(exc_return);
    if(number == 10)
    {
      LOG(control());
    }
    LOG((uint32_t)frame);
  }
  if(number == 12)
  {
    LOG(frame[6]);
  }
}

void hardfault_dispatch(uint32_t const* frame)
{
  LOG(0xC10);
  LOG(REG32(SCB_HFSR));
  LOG(frame[6]);
  REG32(SCB_HFSR) = REG32(SCB_HFSR);
}

static void pendsv_handler(void)
{
  LOG(0xE14);
}

static void nmi_handler(void)
{
  LOG(0xE1D);
}

static void irq0_handler(void)
{
  LOG(0x300);
  __asm__ volatile("svc #7" ::: "memory");
  LOG(0x301);
}

static void irq1_handler(void)
{
  LOG(0xE11);
  REG32(SCB_ICSR) = 1u << 28;
  __asm__ volatile("dsb\nisb" ::: "memory");
}

static void irq3_handler(void)
{
  LOG(0xE13);
}

static void spin_loop(void)
{
  while(REG32(UNMODELLED_STA) != 0)
  {
    __asm__ volatile("svc #9" ::: "memory");
  }
}

__attribute__((noipa)) void svc_setup(void)
{
  for(unsigned number = 0; number < VECTORS; ++number)
  {
    ram_vector[number] = (uint32_t)(number < 24 ? vectors[number] : vectors[2]);
  }
  ram_vector[2]      = (uint32_t)nmi_handler;
  ram_vector[3]      = (uint32_t)hardfault_entry;
  ram_vector[11]     = (uint32_t)svc_entry;
  ram_vector[14]     = (uint32_t)pendsv_handler;
  ram_vector[16 + 0] = (uint32_t)irq0_handler;
  ram_vector[16 + 1] = (uint32_t)irq1_handler;
  ram_vector[16 + 3] = (uint32_t)irq3_handler;
  ram_vector[16 + 5] = (uint32_t)spin_loop;
  REG32(SCB_VTOR)    = (uint32_t)ram_vector;
  REG32(SCB_CPACR) |= 0xFu << 20;
  REG8(SCB_SHPR(11)) = 0x40;
  REG8(SCB_SHPR(14)) = 0xE0;
  REG8(NVIC_IPR(0))  = 0x80;
  REG8(NVIC_IPR(1))  = 0x40;
  REG8(NVIC_IPR(3))  = 0x80;
  REG8(NVIC_IPR(5))  = 0x80;
  REG32(NVIC_ISER0)  = 0x2Bu;
  __asm__ volatile("dsb\nisb\nbkpt #1" ::: "memory");
}

__attribute__((noipa)) void svc_basic(void)
{
  __asm__ volatile("svc #5" ::: "memory");
  LOG(0x8FF);
  __asm__ volatile("bkpt #1" ::: "memory");
}

__attribute__((noipa)) void svc_handler_mode(void)
{
  pend(0);
  __asm__ volatile("bkpt #1" ::: "memory");
}

__attribute__((noipa)) void pendsv_basic(void)
{
  LOG(0xE00);
  REG32(SCB_ICSR) = 1u << 28;
  __asm__ volatile("dsb\nisb" ::: "memory");
  LOG(0xE01);
  __asm__ volatile("bkpt #1" ::: "memory");
}

__attribute__((noipa)) void pendsv_tail(void)
{
  pend(1);
  LOG(0xE02);
  __asm__ volatile("bkpt #1" ::: "memory");
}

__attribute__((noipa)) void pendsv_clear(void)
{
  __asm__ volatile("cpsid i" ::: "memory");
  REG32(SCB_ICSR) = 1u << 28;
  REG32(SCB_ICSR) = 1u << 27;
  __asm__ volatile("dsb\nisb\ncpsie i\nisb" ::: "memory");
  LOG(0xE03);
  __asm__ volatile("bkpt #1" ::: "memory");
}

__attribute__((noipa)) void nmi_run(void)
{
  __asm__ volatile("msr faultmask, %0\nisb" ::"r"(1) : "memory");
  REG32(SCB_ICSR) = 1u << 31;
  __asm__ volatile("dsb\nisb" ::: "memory");
  LOG(0xE04);
  __asm__ volatile("msr faultmask, %0\nisb\nbkpt #1" ::"r"(0) : "memory");
}

__attribute__((noipa)) void stir_run(void)
{
  REG32(NVIC_STIR) = 3;
  __asm__ volatile("dsb\nisb" ::: "memory");
  LOG(0xE05);
  __asm__ volatile("bkpt #1" ::: "memory");
}

__attribute__((noipa)) void stir_unpriv(void)
{
  __asm__ volatile("msr control, %0\nisb" ::"r"(1) : "memory");
  REG32(NVIC_STIR) = 3;
  __asm__ volatile("dsb\nisb" ::: "memory");
  LOG(0xE06);
  __asm__ volatile("bkpt #1" ::: "memory");
}

__attribute__((noipa)) void stir_allowed(void)
{
  REG32(SCB_CCR) |= 1u << 1;
  __asm__ volatile("msr control, %0\nisb" ::"r"(1) : "memory");
  REG32(NVIC_STIR) = 3;
  __asm__ volatile("dsb\nisb" ::: "memory");
  LOG(0xE07);
  __asm__ volatile("bkpt #1" ::: "memory");
}

__attribute__((noipa)) void spin_svc(void)
{
  spin_loop();
  __asm__ volatile("bkpt #1" ::: "memory");
}

__attribute__((noipa)) void spin_svc_handler(void)
{
  pend(5);
  __asm__ volatile("bkpt #1" ::: "memory");
}

void switch_step(void)
{
  if(log_len >= 10)
  {
    __asm__ volatile("bkpt #1" ::: "memory");
  }
  REG32(SCB_ICSR) = 1u << 28;
  __asm__ volatile("dsb\nisb" ::: "memory");
}

void switch_prepare(void)
{
  uint32_t* const a = &stack_a[256] - 8;
  for(unsigned index = 0; index < 8; ++index)
  {
    a[index] = 0;
  }
  a[6] = (uint32_t)task_a & ~1u;
  a[7] = 0x01000000u;

  uint32_t* const b = &stack_b[256] - 8 - 9;
  for(unsigned index = 0; index < 17; ++index)
  {
    b[index] = 0;
  }
  b[8]  = 0xFFFFFFFDu;
  b[15] = (uint32_t)task_b & ~1u;
  b[16] = 0x01000000u;

  task_sp[0]     = (uint32_t)a;
  task_sp[1]     = (uint32_t)b;
  current_task   = 0;
  ram_vector[11] = (uint32_t)switch_svc;
  ram_vector[14] = (uint32_t)switch_pendsv;
  __asm__ volatile("dsb\nisb" ::: "memory");
}
