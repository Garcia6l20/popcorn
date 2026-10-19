#include "secure_entry.h"
#include "stm32u5xx.h"

#include <arm_cmse.h>
#include <stdint.h>

#define CPACR_CP10_CP11 (0xFu << 20)
#define AIRCR_VECTKEY   (0x05FAu << SCB_AIRCR_VECTKEY_Pos)
#define FLASH_REGISTERS 0x400u
#define MPCBB_BLOCK     512u
#define MPCBB_WORD      32u
#define FP_REGISTERS    32u
#define FP_PATTERN      0x5EC00000u

typedef void (*nonsecure_reset)(void) __attribute__((cmse_nonsecure_call));

extern uint32_t const g_pfnVectors[];
extern uint32_t const __sgstubs_start[];
extern uint32_t const __sgstubs_end[];
extern uint32_t const __nonsecure_flash_start[];
extern uint32_t const __nonsecure_flash_end[];
extern uint32_t const __nonsecure_ram_end[];

uint32_t          secure_counter = 100u;
volatile uint32_t secure_illegal_accesses;
volatile uint32_t secure_ticks;
volatile uint32_t secure_fault_sfsr;
volatile uint32_t secure_fault_sfar;

void              SystemInit(void);
void              SysTick_Handler(void);
void              GTZC_IRQHandler(void);
void              SecureFault_Handler(void);
void              HardFault_Handler(void);
[[noreturn]] void secure_fault_spin(void);
[[noreturn]] void secure_hard_fault_spin(void);

void SystemInit(void)
{
  SCB->CPACR |= CPACR_CP10_CP11;
  SCB_NS->CPACR |= CPACR_CP10_CP11;
  FPU->FPCCR |= FPU_FPCCR_TS_Msk;
}

void SysTick_Handler(void)
{
  ++secure_ticks;
}

void GTZC_IRQHandler(void)
{
  if(GTZC_TZIC1_S->SR4 & GTZC_TZIC1_SR4_SRAM1_Msk)
  {
    GTZC_TZIC1_S->FCR4 = GTZC_TZIC1_FCR4_SRAM1_Msk;
    ++secure_illegal_accesses;
  }
}

__attribute__((noipa)) void secure_fault_spin(void)
{
  for(;;)
  {
  }
}

__attribute__((noipa)) void secure_hard_fault_spin(void)
{
  for(;;)
  {
  }
}

void SecureFault_Handler(void)
{
  secure_fault_sfsr = SAU->SFSR;
  secure_fault_sfar = SAU->SFAR;
  secure_fault_spin();
}

void HardFault_Handler(void)
{
  secure_hard_fault_spin();
}

SECURE_ENTRY uint32_t secure_counter_add(uint32_t amount)
{
  secure_counter += amount;
  return secure_counter;
}

SECURE_ENTRY uint32_t secure_read_flag(void)
{
  return secure_illegal_accesses;
}

SECURE_ENTRY uint32_t secure_fp_check(nonsecure_callback callback)
{
  uint32_t pattern[FP_REGISTERS];
  uint32_t after[FP_REGISTERS];
  for(uint32_t index = 0; index < FP_REGISTERS; ++index)
  {
    pattern[index] = FP_PATTERN + index;
  }
  __asm volatile("vldmia %0, {s0-s31}"
                 :
                 : "r"(pattern)
                 : "memory", "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7", "s8", "s9", "s10",
                   "s11", "s12", "s13", "s14", "s15", "s16", "s17", "s18", "s19", "s20", "s21",
                   "s22", "s23", "s24", "s25", "s26", "s27", "s28", "s29", "s30", "s31");
  callback();
  __asm volatile("vstmia %0, {s0-s31}" : : "r"(after) : "memory");
  uint32_t intact = 0;
  for(uint32_t index = 0; index < FP_REGISTERS; ++index)
  {
    intact += after[index] == pattern[index];
  }
  return intact;
}

SECURE_ENTRY void secure_tick_start(uint32_t reload)
{
  SysTick_Config(reload);
}

SECURE_ENTRY void secure_undefined(void)
{
  __asm volatile("udf #0");
}

static void sau_region(uint32_t number, uintptr_t start, uintptr_t end, uint32_t attributes)
{
  SAU->RNR  = number;
  SAU->RBAR = start & SAU_RBAR_BADDR_Msk;
  SAU->RLAR = ((end - 1u) & SAU_RLAR_LADDR_Msk) | attributes | SAU_RLAR_ENABLE_Msk;
}

static void sau_setup(void)
{
  sau_region(0, (uintptr_t)__sgstubs_start, (uintptr_t)__sgstubs_end, SAU_RLAR_NSC_Msk);
  sau_region(1, (uintptr_t)__nonsecure_flash_start, (uintptr_t)__nonsecure_flash_end, 0);
  sau_region(2, SRAM1_BASE_NS, SRAM1_BASE_NS + SRAM1_SIZE, 0);
  sau_region(3, PERIPH_BASE_NS, FLASH_R_BASE_NS, 0);
  sau_region(4, FLASH_R_BASE_NS + FLASH_REGISTERS, PERIPH_BASE_S, 0);
  SAU->CTRL = SAU_CTRL_ENABLE_Msk;
}

static void mpcbb_setup(void)
{
  uint32_t const released = ((uintptr_t)__nonsecure_ram_end - SRAM1_BASE_NS) / MPCBB_BLOCK;
  for(uint32_t word = 0; word < SRAM1_SIZE / MPCBB_BLOCK / MPCBB_WORD; ++word)
  {
    uint32_t secure = 0;
    for(uint32_t bit = 0; bit < MPCBB_WORD; ++bit)
    {
      if(word * MPCBB_WORD + bit >= released)
      {
        secure |= 1u << bit;
      }
    }
    GTZC_MPCBB1_S->SECCFGR[word] = secure;
  }
}

int main(void)
{
  SCB->VTOR = (uintptr_t)g_pfnVectors;
  SCB->SHCSR |= SCB_SHCSR_SECUREFAULTENA_Msk;
  sau_setup();
  RCC->AHB1ENR |= RCC_AHB1ENR_GTZC1EN;
  RCC->AHB2ENR1 |= RCC_AHB2ENR1_GPIOAEN;
  mpcbb_setup();
  GTZC_TZIC1_S->IER4 = GTZC_TZIC1_IER4_SRAM1_Msk;
  NVIC_EnableIRQ(GTZC_IRQn);
  GPIOA->SECCFGR &= ~GPIO_SECCFGR_SEC5;
  SCB->NSACR |= SCB_NSACR_CP10_Msk | SCB_NSACR_CP11_Msk;
  SCB->AIRCR   = AIRCR_VECTKEY | (SCB->AIRCR & ~SCB_AIRCR_VECTKEY_Msk) | SCB_AIRCR_BFHFNMINS_Msk;
  SCB_NS->VTOR = (uintptr_t)__nonsecure_flash_start;
  __DSB();
  __ISB();
  __TZ_set_MSP_NS(__nonsecure_flash_start[0]);
  nonsecure_reset const reset = (nonsecure_reset)cmse_nsfptr_create(__nonsecure_flash_start[1]);
  reset();
  for(;;)
  {
  }
}
