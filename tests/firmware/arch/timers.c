#include "arch_test.h"

#define SCB_ICSR        0xE000ED04u
#define SCB_VTOR        0xE000ED08u
#define SCB_SCR         0xE000ED10u
#define SCB_SHPR(n)     (0xE000ED18u + (n) - 4u)
#define NVIC_ISER0      0xE000E100u
#define NVIC_ICER0      0xE000E180u
#define SYST_CSR        0xE000E010u
#define SYST_RVR        0xE000E014u
#define SYST_CVR        0xE000E018u
#define TESTDEV_RW      0x40010008u
#define UNMODELLED_STAT 0x40020004u

#define VECTORS 48

extern void const* const vectors[24];

__attribute__((aligned(512))) uint32_t ram_vector[VECTORS];
volatile uint32_t                      ticks;

static void systick_handler(void)
{
  ticks = ticks + 1u;
}

static void spin_systick(void)
{
  (void)REG32(SYST_CSR);
  (void)REG32(TESTDEV_RW);
  REG32(SCB_ICSR) = 1u << 28;
}

static void spin_pendsv(void)
{
  (void)REG32(TESTDEV_RW);
}

static void start_systick(uint32_t reload, uint32_t control)
{
  REG32(SYST_RVR) = reload;
  REG32(SYST_CVR) = 0;
  REG32(SYST_CSR) = control;
}

__attribute__((noipa)) void timers_setup(void)
{
  for(unsigned number = 0; number < VECTORS; ++number)
  {
    ram_vector[number] = (uint32_t)(number < 24 ? vectors[number] : vectors[2]);
  }
  ram_vector[15]     = (uint32_t)systick_handler;
  REG32(SCB_VTOR)    = (uint32_t)ram_vector;
  REG8(SCB_SHPR(15)) = 0x80;
  REG8(SCB_SHPR(14)) = 0xE0;
  __asm__ volatile("dsb\nisb\nbkpt #1" ::: "memory");
}

__attribute__((noipa)) void tick_1khz(void)
{
  start_systick(3999, 7);
  for(;;)
  {
    __asm__ volatile("wfi" ::: "memory");
  }
}

__attribute__((noipa)) void countflag(void)
{
  start_systick(999, 5);
  for(unsigned round = 0; round < 5; ++round)
  {
    uint32_t polls = 0;
    while((REG32(SYST_CSR) & (1u << 16)) == 0)
    {
      ++polls;
    }
    LOG(polls);
  }
  __asm__ volatile("wfi\nbkpt #1" ::: "memory");
}

__attribute__((noipa)) void wake_masked(void)
{
  __asm__ volatile("cpsid i" ::: "memory");
  start_systick(3999, 7);
  __asm__ volatile("wfi" ::: "memory");
  LOG(ticks);
  __asm__ volatile("cpsie i\nisb" ::: "memory");
  LOG(ticks);
  __asm__ volatile("bkpt #1" ::: "memory");
}

__attribute__((noipa)) void moved_here(void)
{
  LOG(0x3E7);
  __asm__ volatile("bkpt #1" ::: "memory");
}

__attribute__((noipa)) void spin_tick(void)
{
  ram_vector[14] = (uint32_t)spin_pendsv;
  ram_vector[15] = (uint32_t)spin_systick;
  start_systick(999, 7);
  while(REG32(UNMODELLED_STAT) != 0)
  {
  }
  __asm__ volatile("bkpt #1" ::: "memory");
}

uint32_t thread_counter;

__attribute__((noipa)) void wfe_basic(void)
{
  start_systick(3999, 7);
  LOG(ticks);
  __asm__ volatile("wfe" ::: "memory");
  LOG(ticks);
  __asm__ volatile("bkpt #1" ::: "memory");
}

__attribute__((noipa)) void wfe_sev(void)
{
  start_systick(3999, 7);
  __asm__ volatile("sev\nwfe" ::: "memory");
  LOG(ticks);
  __asm__ volatile("bkpt #1" ::: "memory");
}

__attribute__((noipa)) void wfe_sev_twice(void)
{
  start_systick(3999, 7);
  __asm__ volatile("sev\nwfe\nwfe" ::: "memory");
  LOG(ticks);
  __asm__ volatile("bkpt #1" ::: "memory");
}

__attribute__((noipa)) void yield_loop(void)
{
  for(uint32_t round = 0; round < 100; ++round)
  {
    __asm__ volatile("yield" ::: "memory");
    LOG(round);
  }
  __asm__ volatile("bkpt #1" ::: "memory");
}

__attribute__((noipa)) void wfe_sevonpend(void)
{
  REG32(NVIC_ICER0) = 1u << 6;
  REG32(SCB_SCR) |= 1u << 4;
  __asm__ volatile("dsb\nisb\nbkpt #2\nwfe" ::: "memory");
  LOG(0x5E);
  __asm__ volatile("bkpt #1" ::: "memory");
}

__attribute__((noipa)) void sleep_on_exit(void)
{
  REG32(SCB_SCR) |= 1u << 1;
  start_systick(3999, 7);
  __asm__ volatile("dsb\nisb\nwfi" ::: "memory");
  for(;;)
  {
    thread_counter = thread_counter + 1u;
  }
}

static void wfe_masked(uint32_t sevonpend)
{
  __asm__ volatile("cpsid i" ::: "memory");
  if(sevonpend != 0)
  {
    REG32(SCB_SCR) |= 1u << 4;
  }
  else
  {
    REG32(SCB_SCR) &= ~(1u << 4);
  }
  REG32(NVIC_ISER0) = 1u << 7;
  __asm__ volatile("dsb\nisb\nbkpt #2\nwfe" ::: "memory");
  LOG(0x7E);
  __asm__ volatile("bkpt #1" ::: "memory");
}

__attribute__((noipa)) void wfe_primask(void)
{
  wfe_masked(0);
}

__attribute__((noipa)) void wfe_primask_sevonpend(void)
{
  wfe_masked(1);
}
