#include "arch_test.h"

#define NVIC_ISER0  0xE000E100u
#define NVIC_ISPR0  0xE000E200u
#define NVIC_IPR(n) (0xE000E400u + (n))
#define SCB_VTOR    0xE000ED08u
#define SCB_AIRCR   0xE000ED0Cu
#define SCB_SHCSR   0xE000ED24u
#define SCB_CPACR   0xE000ED88u

#define VECTORS 48

extern void const* const vectors[24];

void     fpu_check_irq(void);
void     psp_irq(void);
void     it_irq(void);
uint32_t fpu_check(void);
void     psp_section(void);
uint32_t it_irq_test(void);

__attribute__((aligned(512))) uint32_t ram_vector[VECTORS];

static void pend(unsigned irq)
{
  REG32(NVIC_ISPR0) = 1u << irq;
  __asm__ volatile("dsb\nisb" ::: "memory");
}

static void set_vec(unsigned number, void (*handler)(void))
{
  ram_vector[number] = (uint32_t)handler;
}

static void irq0_handler(void)
{
  LOG(0x300);
  pend(1);
  LOG(0x301);
}

static void irq1_handler(void)
{
  LOG(0x310);
}

static void irq2_handler(void)
{
  LOG(0x400);
  pend(3);
  LOG(0x401);
}

static void irq3_handler(void)
{
  LOG(0x410);
}

static void irq4_handler(void)
{
  LOG(0x440);
}

static void irq5_handler(void)
{
  LOG(0x450);
}

static void irq6_handler(void)
{
  LOG(0x510);
}

static void irq7_handler(void)
{
  LOG(0x610);
}

static void group4_handler(void)
{
  LOG(0xD40);
  pend(5);
  LOG(0xD41);
}

static void group5_handler(void)
{
  LOG(0xD50);
}

void setup_vectors(void)
{
  for(unsigned number = 0; number < VECTORS; ++number)
  {
    ram_vector[number] = (uint32_t)(number < 24 ? vectors[number] : vectors[2]);
  }
  set_vec(16 + 0, irq0_handler);
  set_vec(16 + 1, irq1_handler);
  set_vec(16 + 2, irq2_handler);
  set_vec(16 + 3, irq3_handler);
  set_vec(16 + 4, irq4_handler);
  set_vec(16 + 5, irq5_handler);
  set_vec(16 + 6, irq6_handler);
  set_vec(16 + 7, irq7_handler);
  set_vec(16 + 8, fpu_check_irq);
  set_vec(16 + 9, psp_irq);
  set_vec(16 + 10, it_irq);
  REG32(SCB_VTOR) = (uint32_t)ram_vector;
  REG32(SCB_CPACR) |= 0xFu << 20;
  REG32(SCB_SHCSR) |= (1u << 16) | (1u << 17) | (1u << 18);
  __asm__ volatile("dsb\nisb" ::: "memory");
}

static void grouping(uint32_t prigroup)
{
  REG32(SCB_AIRCR)  = 0x05FA0000u | prigroup << 8;
  REG8(NVIC_IPR(4)) = 0x40;
  REG8(NVIC_IPR(5)) = 0x20;
  pend(4);
}

__attribute__((noipa)) void exceptions_run(void)
{
  setup_vectors();
  LOG(0x100);

  REG32(NVIC_ISER0) = 0x7FF;
  REG8(NVIC_IPR(0)) = 0x80;
  REG8(NVIC_IPR(1)) = 0x40;
  pend(0);
  LOG(0x3FF);

  REG8(NVIC_IPR(2)) = 0x80;
  REG8(NVIC_IPR(3)) = 0x80;
  pend(2);
  LOG(0x4FF);

  REG8(NVIC_IPR(4)) = 0x80;
  REG8(NVIC_IPR(5)) = 0x40;
  __asm__ volatile("cpsid i" ::: "memory");
  REG32(NVIC_ISPR0) = (1u << 4) | (1u << 5);
  __asm__ volatile("dsb\nisb\ncpsie i" ::: "memory");
  LOG(0x4FE);

  REG8(NVIC_IPR(6)) = 0x80;
  __asm__ volatile("msr basepri, %0\nisb" ::"r"(0x40) : "memory");
  pend(6);
  LOG(0x500);
  __asm__ volatile("msr basepri, %0\nisb" ::"r"(0x80) : "memory");
  LOG(0x501);
  __asm__ volatile("msr basepri, %0\nisb" ::"r"(0) : "memory");
  LOG(0x502);

  REG8(NVIC_IPR(7)) = 0x80;
  __asm__ volatile("cpsid i" ::: "memory");
  pend(7);
  LOG(0x600);
  __asm__ volatile("cpsie i" ::: "memory");
  LOG(0x601);

  LOG(0x700 | fpu_check());

  psp_section();
  LOG(0x9FF);

  REG8(NVIC_IPR(0)) = 0x00;
  __asm__ volatile("msr faultmask, %0\nisb" ::"r"(1) : "memory");
  pend(0);
  LOG(0xC00);
  __asm__ volatile("msr faultmask, %0\nisb" ::"r"(0) : "memory");
  LOG(0xC01);

  set_vec(16 + 4, group4_handler);
  set_vec(16 + 5, group5_handler);
  grouping(6);
  grouping(0);

  LOG(0xA00 | it_irq_test());

  __asm__ volatile("bkpt #1" ::: "memory");
}

void clobber_irq(void);
void nested_body(void);
void nested_fp_body(void);
void tail_a(void);
void tail_b(void);
void faultmask_irq(void);

static void latch_irq(void)
{
  LOG(0x330);
  (void)REG32(0x40010058u);
}

__attribute__((noipa)) void probe_setup(void)
{
  setup_vectors();
  set_vec(16 + 11, clobber_irq);
  set_vec(16 + 12, nested_body);
  set_vec(16 + 13, nested_fp_body);
  set_vec(16 + 14, tail_a);
  set_vec(16 + 15, tail_b);
  set_vec(16 + 16, faultmask_irq);
  set_vec(16 + 3, latch_irq);
  REG8(NVIC_IPR(3))  = 0x80;
  REG8(NVIC_IPR(11)) = 0x40;
  REG8(NVIC_IPR(12)) = 0xC0;
  REG8(NVIC_IPR(13)) = 0xC0;
  REG8(NVIC_IPR(14)) = 0x80;
  REG8(NVIC_IPR(15)) = 0x80;
  REG8(NVIC_IPR(16)) = 0x80;
  REG32(NVIC_ISER0)  = 0x1F808u;
  __asm__ volatile("dsb\nisb\nbkpt #1" ::: "memory");
}

__attribute__((noipa)) void latch_pulse(void)
{
  REG32(0x4001005Cu) = 1u;
  __asm__ volatile("dsb\nisb\nnop\nnop\nbkpt #1" ::: "memory");
}

__attribute__((noipa)) void latch_held(void)
{
  REG32(0x4001005Cu) = 2u;
  __asm__ volatile("dsb\nisb\nnop\nnop\nbkpt #1" ::: "memory");
}

uint32_t icsr_seen[2];

static void icsr_inner(void)
{
  icsr_seen[1] = REG32(0xE000ED04u);
}

static void icsr_outer(void)
{
  pend(18);
  pend(19);
  icsr_seen[0] = REG32(0xE000ED04u);
}

static void icsr_low(void)
{}

__attribute__((noipa)) void icsr_probe(void)
{
  set_vec(16 + 17, icsr_outer);
  set_vec(16 + 18, icsr_low);
  set_vec(16 + 19, icsr_inner);
  REG8(NVIC_IPR(17)) = 0x40;
  REG8(NVIC_IPR(18)) = 0xC0;
  REG8(NVIC_IPR(19)) = 0x20;
  REG32(NVIC_ISER0)  = 0xE0000u;
  pend(17);
  __asm__ volatile("bkpt #1" ::: "memory");
}
