#include "arch_test.h"

#define NVIC_ISER0      0xE000E100u
#define NVIC_ISPR0      0xE000E200u
#define NVIC_IPR(n)     (0xE000E400u + (n))
#define SCB_CPUID       0xE000ED00u
#define SCB_VTOR        0xE000ED08u
#define SCB_SHCSR       0xE000ED24u
#define SCB_CFSR        0xE000ED28u
#define SCB_HFSR        0xE000ED2Cu
#define SCB_BFAR        0xE000ED38u
#define TESTDEV_RW      0x40010008u
#define TESTDEV_DATA    0x40010058u
#define UNMODELLED_STAT 0x40020004u

#define VECTORS 48

extern void const* const vectors[24];

void hardfault_entry(void);
void memmanage_entry(void);
void busfault_entry(void);
void usagefault_entry(void);
void hardfault_udf(void);
void usage_skip(void);
void bad_return_irq(void);
void bad_return_v8_irq(void);

__attribute__((aligned(512))) uint32_t ram_vector[VECTORS];
uint32_t                               unpriv_read;

void fault_log(uint32_t const* frame, uint32_t code)
{
  LOG(code);
  LOG(REG32(SCB_CFSR));
  LOG(REG32(SCB_BFAR));
  LOG(REG32(SCB_HFSR));
  LOG(frame[6]);
  REG32(SCB_CFSR) = REG32(SCB_CFSR);
  REG32(SCB_HFSR) = REG32(SCB_HFSR);
  __asm__ volatile("bkpt #1" ::: "memory");
}

void usage_skip_dispatch(uint32_t* frame)
{
  (void)REG32(TESTDEV_RW);
  REG32(SCB_CFSR) = REG32(SCB_CFSR);
  frame[6] += 2u;
}

static void enable_faults(uint32_t enabled)
{
  if(enabled != 0)
  {
    REG32(SCB_SHCSR) |= 0x70000u;
  }
  else
  {
    REG32(SCB_SHCSR) &= ~0x70000u;
  }
  __asm__ volatile("dsb\nisb" ::: "memory");
}

static void pend(unsigned irq)
{
  REG32(NVIC_ISPR0) = 1u << irq;
  __asm__ volatile("dsb\nisb" ::: "memory");
}

__attribute__((noipa)) void faults_setup(void)
{
  for(unsigned number = 0; number < VECTORS; ++number)
  {
    ram_vector[number] = (uint32_t)(number < 24 ? vectors[number] : vectors[2]);
  }
  ram_vector[3]     = (uint32_t)hardfault_entry;
  ram_vector[4]     = (uint32_t)memmanage_entry;
  ram_vector[5]     = (uint32_t)busfault_entry;
  ram_vector[6]     = (uint32_t)usagefault_entry;
  ram_vector[16]    = (uint32_t)bad_return_irq;
  ram_vector[17]    = (uint32_t)bad_return_v8_irq;
  REG32(SCB_VTOR)   = (uint32_t)ram_vector;
  REG8(NVIC_IPR(0)) = 0x80;
  REG8(NVIC_IPR(1)) = 0x80;
  REG32(NVIC_ISER0) = 3u;
  __asm__ volatile("dsb\nisb\nbkpt #1" ::: "memory");
}

__attribute__((noipa)) void udf(void)
{
  enable_faults(1);
  __asm__ volatile(".global udf_at\n.type udf_at, %%function\nudf_at:\nudf #1" ::: "memory");
}

__attribute__((noipa)) void udf_forced(void)
{
  enable_faults(0);
  __asm__ volatile(
      ".global udf_forced_at\n.type udf_forced_at, %%function\nudf_forced_at:\nudf #1" ::
          : "memory");
}

__attribute__((noipa)) void unmapped_read(void)
{
  enable_faults(1);
  __asm__ volatile("ldr r0, =0x30000000\n"
                   ".global unmapped_read_at\n.type unmapped_read_at, %%function\n"
                   "unmapped_read_at:\nldr r0, [r0]\nb .\n.ltorg" ::
                       : "r0", "memory");
}

__attribute__((noipa)) void unmapped_write(void)
{
  enable_faults(1);
  __asm__ volatile("ldr r0, =0x30000000\n"
                   ".global unmapped_write_at\n.type unmapped_write_at, %%function\n"
                   "unmapped_write_at:\nstr r0, [r0]\nb .\n.ltorg" ::
                       : "r0", "memory");
}

__attribute__((noipa)) void unmapped_fetch(void)
{
  enable_faults(1);
  __asm__ volatile("ldr r0, =0x30000001\nblx r0\nb .\n.ltorg" ::: "r0", "lr", "memory");
}

__attribute__((noipa)) void flash_write(void)
{
  enable_faults(1);
  __asm__ volatile("ldr r0, =0x08000400\n"
                   ".global flash_write_at\n.type flash_write_at, %%function\n"
                   "flash_write_at:\nstr r0, [r0]\nb .\n.ltorg" ::
                       : "r0", "memory");
}

__attribute__((noipa)) void model_bus_error(void)
{
  enable_faults(1);
  REG32(TESTDEV_DATA) = 1u;
  __asm__ volatile("b 1f\n"
                   ".global model_bus_error_next\n.type model_bus_error_next, %%function\n"
                   "model_bus_error_next:\n1:\nnop\nb ." ::
                       : "memory");
}

__attribute__((noipa)) void fault_in_hardfault(void)
{
  enable_faults(0);
  ram_vector[3] = (uint32_t)hardfault_udf;
  __asm__ volatile("dsb\nisb\nudf #1" ::: "memory");
}

__attribute__((noipa)) void faultmask_udf(void)
{
  enable_faults(1);
  __asm__ volatile("msr faultmask, %0\nisb\nudf #1" ::"r"(1) : "memory");
}

__attribute__((noipa)) void return_to_reset_lr(void)
{
  enable_faults(1);
  __asm__ volatile("ldr lr, =0xFFFFFFFF\nbx lr\n.ltorg" ::: "lr", "memory");
}

__attribute__((noipa)) void bad_exc_return(void)
{
  enable_faults(1);
  pend(0);
  __asm__ volatile("b ." ::: "memory");
}

__attribute__((noipa)) void stack_overflow_entry(void)
{
  enable_faults(1);
  __asm__ volatile("ldr r0, =0x30000100\nmsr msp, r0\nisb\n"
                   "ldr r1, =0xE000E200\nmovs r2, #1\nstr r2, [r1]\ndsb\nisb\nb .\n.ltorg" ::
                       : "r0", "r1", "r2", "memory");
}

__attribute__((noipa)) void bad_vector(void)
{
  enable_faults(1);
  REG32(SCB_VTOR) = 0x30000000u;
  pend(0);
  __asm__ volatile("b ." ::: "memory");
}

__attribute__((noipa)) void invstate(void)
{
  enable_faults(1);
  __asm__ volatile("ldr r0, =invstate_target\nbic r0, r0, #1\nbx r0\n.ltorg\n"
                   ".global invstate_target\n.type invstate_target, %%function\n"
                   "invstate_target:\nnop\nb ." ::
                       : "r0", "memory");
}

__attribute__((noipa)) void stack_limit(void)
{
  enable_faults(1);
  __asm__ volatile("mov r0, sp\nsub r0, r0, #64\nmsr msplim, r0\nisb\n"
                   "movs r1, #32\n1:\npush {r2}\nsubs r1, #1\nbne 1b\nbkpt #2" ::
                       : "r0", "r1", "memory");
}

__attribute__((noipa)) void deep_stack_run(void)
{
  __asm__ volatile("movs r1, #32\n1:\npush {r2}\nsubs r1, #1\nbne 1b\nadd sp, sp, #128\nbkpt #1" ::
                       : "r1", "memory");
}

__attribute__((noipa)) void unpriv_scs(void)
{
  enable_faults(1);
  __asm__ volatile("msr control, %0\nisb" ::"r"(1) : "memory");
  unpriv_read = REG32(SCB_CPUID);
  __asm__ volatile("bkpt #1" ::: "memory");
}

__attribute__((noipa)) void spin_udf(void)
{
  enable_faults(1);
  ram_vector[6] = (uint32_t)usage_skip;
  __asm__ volatile("dsb\nisb" ::: "memory");
  while(REG32(UNMODELLED_STAT) != 0)
  {
    __asm__ volatile("udf #2" ::: "memory");
  }
  __asm__ volatile("bkpt #1" ::: "memory");
}

__attribute__((noipa)) void bad_exc_return_v8(void)
{
  enable_faults(1);
  pend(1);
  __asm__ volatile("b ." ::: "memory");
}
