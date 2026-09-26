#include "arch_test.h"

#define NVIC_ISER0      0xE000E100u
#define NVIC_ISPR0      0xE000E200u
#define NVIC_IPR(n)     (0xE000E400u + (n))
#define SCB_ICSR        0xE000ED04u
#define SCB_VTOR        0xE000ED08u
#define SCB_SHPR(n)     (0xE000ED18u + (n) - 4u)
#define SCB_CPACR       0xE000ED88u
#define SYST_CSR        0xE000E010u
#define SYST_RVR        0xE000E014u
#define SYST_CVR        0xE000E018u
#define TESTDEV_DATA    0x40010058u
#define TESTDEV_CTRL    0x4001005Cu
#define UNMODELLED_STAT 0x40020004u

#define PENDSVSET   (1u << 28)
#define MAX_SYSCALL 0x40u
#define KERNEL_PRIO 0xE0u
#define IRQ_TESTDEV 3u
#define STACK_WORDS 256u
#define TASKS       3u
#define LONG_DELAY  100000u
#define VECTORS     48

#define TAG(letter, value) (((uint32_t)(letter) << 24) | (value))

enum mode
{
  mode_run,
  mode_crit,
  mode_hung,
  mode_idle,
  mode_busy,
};

enum task_index
{
  task_index_a,
  task_index_idle,
  task_index_b,
};

struct task
{
  uint32_t sp;
  uint32_t psplim;
  uint32_t exc_return;
  uint32_t priority;
  uint32_t wake;
  uint32_t blocked;
};

void              task_a(void);
void              task_b(void);
void              task_idle(void);
void              task_exit(void);
[[noreturn]] void start_scheduler(void);

extern void const* const vectors[24];

__attribute__((aligned(8))) uint32_t task_stacks[TASKS][STACK_WORDS];
struct task                          tasks[TASKS];
struct task*                         current;
volatile uint32_t                    tick;
uint32_t                             rtos_mode;
uint32_t                             crit_done;
uint32_t                             psp_seen[TASKS];
uint32_t                             psp_bad[TASKS];
uint32_t                             fp_checks;
uint32_t                             fp_bad;
uint32_t                             busy_count;

__attribute__((aligned(512))) uint32_t bench_vector[VECTORS];

static void barrier(void)
{
  __asm__ volatile("dsb\nisb" ::: "memory");
}

static void crit_enter(void)
{
  __asm__ volatile("msr basepri, %0\ndsb\nisb" ::"r"(MAX_SYSCALL) : "memory");
}

static void crit_exit(void)
{
  __asm__ volatile("msr basepri, %0\nisb" ::"r"(0) : "memory");
}

static void task_init(enum task_index index, void (*entry)(void), uint32_t priority)
{
  uint32_t* const top     = &task_stacks[index][STACK_WORDS];
  uint32_t* const frame   = top - 8;
  uint32_t* const context = frame - 8;
  for(uint32_t* at = context; at < top; ++at)
  {
    *at = 0;
  }
  frame[5]     = (uint32_t)task_exit;
  frame[6]     = (uint32_t)entry & ~1u;
  frame[7]     = 0x01000000u;
  tasks[index] = (struct task){
      .sp         = (uint32_t)context,
      .psplim     = (uint32_t)&task_stacks[index][0],
      .exc_return = 0xFFFFFFBCu,
      .priority   = priority,
      .wake       = 0,
      .blocked    = 0};
  psp_seen[index] = 0;
  psp_bad[index]  = 0;
}

void rtos_select(void)
{
  struct task* best = &tasks[task_index_idle];
  for(uint32_t index = 0; index < TASKS; ++index)
  {
    if(tasks[index].blocked == 0 && tasks[index].priority > best->priority)
    {
      best = &tasks[index];
    }
  }
  current = best;
}

void SysTick_Handler(void)
{
  (void)REG32(SYST_CSR);
  uint32_t const now    = tick + 1u;
  uint32_t       wanted = 0;
  tick                  = now;
  for(uint32_t index = 0; index < TASKS; ++index)
  {
    if(tasks[index].blocked != 0 && tasks[index].wake == now)
    {
      tasks[index].blocked = 0;
      wanted |= tasks[index].priority > current->priority;
    }
  }
  if(wanted != 0)
  {
    REG32(SCB_ICSR) = PENDSVSET;
  }
}

void IRQ3_Handler(void)
{
  (void)REG32(TESTDEV_DATA);
  LOG(TAG('I', tick));
}

void rtos_delay(uint32_t ticks)
{
  crit_enter();
  current->wake    = tick + ticks;
  current->blocked = 1;
  REG32(SCB_ICSR)  = PENDSVSET;
  crit_exit();
}

void task_probe(enum task_index index, uint32_t psp)
{
  if(psp_seen[index] == 0)
  {
    psp_seen[index] = psp;
  }
  else if(psp_seen[index] != psp)
  {
    ++psp_bad[index];
  }
}

static void critical_section(void)
{
  crit_done = 1;
  crit_enter();
  LOG(TAG('S', tick));
  REG32(TESTDEV_CTRL) = 1u;
  for(volatile uint32_t count = 0; count < 200u; ++count)
  {
  }
  LOG(TAG('E', tick));
  LOG(TAG('P', (REG32(NVIC_ISPR0) >> IRQ_TESTDEV) & 1u));
  crit_exit();
}

void task_a_step(void)
{
  rtos_delay(rtos_mode == mode_idle ? LONG_DELAY : 3u);
  LOG(TAG('A', tick));
  if(rtos_mode == mode_crit && crit_done == 0)
  {
    critical_section();
  }
}

void task_b_step(void)
{
  if(rtos_mode == mode_busy)
  {
    float value = 1.0f;
    for(uint32_t round = 0; round < 64u; ++round)
    {
      value = value * 1.0001f + 0.5f;
    }
    busy_count = busy_count + (uint32_t)value;
    return;
  }
  rtos_delay(rtos_mode == mode_idle ? LONG_DELAY : 5u);
  LOG(TAG('B', tick));
  while(rtos_mode == mode_hung && REG32(UNMODELLED_STAT) != 0)
  {
  }
}

void fp_check(uint32_t seen, uint32_t expected)
{
  ++fp_checks;
  if(seen != expected)
  {
    ++fp_bad;
  }
}

__attribute__((noipa)) int add(int left, int right)
{
  return left + right;
}

[[noreturn]] static void rtos_start(enum mode mode)
{
  rtos_mode = mode;
  crit_done = 0;
  tick      = 0;
  fp_checks = 0;
  fp_bad    = 0;
  REG32(SCB_CPACR) |= 0xFu << 20;
  task_init(task_index_a, task_a, 2);
  task_init(task_index_b, task_b, 1);
  task_init(task_index_idle, task_idle, 0);
  current                     = &tasks[task_index_a];
  REG8(SCB_SHPR(14))          = KERNEL_PRIO;
  REG8(SCB_SHPR(15))          = KERNEL_PRIO;
  REG8(NVIC_IPR(IRQ_TESTDEV)) = 0x60;
  REG32(NVIC_ISER0)           = 1u << IRQ_TESTDEV;
  REG32(SYST_RVR)             = 3999u;
  REG32(SYST_CVR)             = 0;
  REG32(SYST_CSR)             = 7u;
  barrier();
  start_scheduler();
}

__attribute__((noipa)) void rtos_run(void)
{
  rtos_start(mode_run);
}

__attribute__((noipa)) void rtos_crit(void)
{
  rtos_start(mode_crit);
}

__attribute__((noipa)) void rtos_hung(void)
{
  rtos_start(mode_hung);
}

__attribute__((noipa)) void rtos_idle(void)
{
  rtos_start(mode_idle);
}

__attribute__((noipa)) void rtos_busy(void)
{
  rtos_start(mode_busy);
}

static void bench_empty(void)
{}

static void bench_chain(void)
{
  REG32(SCB_ICSR) = PENDSVSET;
}

static void bench_table(void (*irq3)(void))
{
  for(unsigned number = 0; number < VECTORS; ++number)
  {
    bench_vector[number] = (uint32_t)(number < 24 ? vectors[number] : vectors[2]);
  }
  bench_vector[14]               = (uint32_t)bench_empty;
  bench_vector[15]               = (uint32_t)bench_empty;
  bench_vector[16 + IRQ_TESTDEV] = (uint32_t)irq3;
  REG32(SCB_VTOR)                = (uint32_t)bench_vector;
  REG8(SCB_SHPR(14))             = KERNEL_PRIO;
  REG8(SCB_SHPR(15))             = KERNEL_PRIO;
  REG8(NVIC_IPR(IRQ_TESTDEV))    = 0x60;
  REG32(NVIC_ISER0)              = 1u << IRQ_TESTDEV;
  barrier();
}

static void bench_alu(uint32_t loops)
{
  __asm__ volatile("1:\nsubs %0, %0, #1\nbne 1b\nbkpt #1" : "+r"(loops)::"memory");
}

__attribute__((noipa)) void bench_loop(uint32_t loops)
{
  bench_table(bench_empty);
  bench_alu(loops);
}

__attribute__((noipa)) void bench_irq_rate(uint32_t loops)
{
  bench_table(bench_empty);
  REG32(SYST_RVR) = 999u;
  REG32(SYST_CVR) = 0;
  REG32(SYST_CSR) = 7u;
  bench_alu(loops);
}

__attribute__((noipa)) void bench_masked(uint32_t loops)
{
  bench_table(bench_empty);
  crit_enter();
  REG32(NVIC_ISPR0) = 1u << IRQ_TESTDEV;
  barrier();
  bench_alu(loops);
}

static void bench_irqs(uint32_t count)
{
  for(uint32_t round = 0; round < count; ++round)
  {
    REG32(NVIC_ISPR0) = 1u << IRQ_TESTDEV;
    barrier();
  }
  __asm__ volatile("bkpt #1" ::: "memory");
}

__attribute__((noipa)) void bench_plain_irq(uint32_t count)
{
  bench_table(bench_empty);
  bench_irqs(count);
}

__attribute__((noipa)) void bench_tail_chain(uint32_t count)
{
  bench_table(bench_chain);
  bench_irqs(count);
}
