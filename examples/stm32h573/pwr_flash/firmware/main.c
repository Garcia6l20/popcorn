#include "main.h"

#include "stm32h5xx.h"
#include "stm32h5xx_ll_bus.h"
#include "stm32h5xx_ll_icache.h"
#include "stm32h5xx_ll_pwr.h"
#include "stm32h5xx_ll_rcc.h"
#include "stm32h5xx_ll_system.h"
#include "stm32h5xx_ll_utils.h"

#define UNLOCK_KEY1  0x45670123u
#define UNLOCK_KEY2  0xCDEF89ABu
#define DATA_SECTOR  127u
#define DATA_ADDRESS 0x081FE000u
#define FLASH_ERRORS (FLASH_SR_WRPERR | FLASH_SR_PGSERR | FLASH_SR_STRBERR | FLASH_SR_INCERR)

volatile uint32_t                erase_status;
volatile uint32_t                erase_end_flag;
volatile uint32_t                program_status;
volatile uint32_t                stray_status;
volatile struct device_signature signature;

static uint32_t const record[4] = {0xC0FFEE00u, 0x12345678u, 0x9ABCDEF0u, 0x0BADF00Du};

__attribute__((noipa)) void clock_configured(void)
{}

__attribute__((noipa)) void sector_erased(void)
{}

__attribute__((noipa)) void record_programmed(void)
{}

__attribute__((noipa)) void flash_done(void)
{}

__attribute__((noipa)) void signature_read(void)
{}

__attribute__((noipa)) void set_latency(uint32_t wait_states)
{
  LL_FLASH_SetLatency(wait_states);
}

static void SystemClock_Config(void)
{
  LL_PWR_SetRegulVoltageScaling(LL_PWR_REGU_VOLTAGE_SCALE0);
  while(LL_PWR_IsActiveFlag_VOS() == 0)
  {
  }
  set_latency(LL_FLASH_LATENCY_5);
  while(LL_FLASH_GetLatency() != LL_FLASH_LATENCY_5)
  {
  }
  MODIFY_REG(FLASH->ACR, FLASH_ACR_WRHIGHFREQ, FLASH_ACR_WRHIGHFREQ_1);
  while(READ_BIT(FLASH->ACR, FLASH_ACR_WRHIGHFREQ) != FLASH_ACR_WRHIGHFREQ_1)
  {
  }

  LL_RCC_HSI_SetDivider(LL_RCC_HSI_DIV_2);
  while(LL_RCC_HSI_IsDividerReady() == 0)
  {
  }
  LL_RCC_PLL1_ConfigDomain_SYS(LL_RCC_PLL1SOURCE_HSI, 2, 31, 2);
  LL_RCC_PLL1P_Enable();
  LL_RCC_PLL1_SetVCOInputRange(LL_RCC_PLLINPUTRANGE_8_16);
  LL_RCC_PLL1_SetVCOOutputRange(LL_RCC_PLLVCORANGE_WIDE);
  LL_RCC_PLL1FRACN_Enable();
  LL_RCC_PLL1_SetFRACN(2048);
  LL_RCC_PLL1_Enable();
  while(LL_RCC_PLL1_IsReady() == 0)
  {
  }

  LL_RCC_SetAHBPrescaler(LL_RCC_SYSCLK_DIV_2);
  LL_RCC_SetSysClkSource(LL_RCC_SYS_CLKSOURCE_PLL1);
  while(LL_RCC_GetSysClkSource() != LL_RCC_SYS_CLKSOURCE_STATUS_PLL1)
  {
  }
  LL_RCC_SetAHBPrescaler(LL_RCC_SYSCLK_DIV_1);
  SystemCoreClockUpdate();
  LL_ICACHE_Enable();
  clock_configured();
}

static void flash_unlock(void)
{
  if(READ_BIT(FLASH->NSCR, FLASH_CR_LOCK) != 0u)
  {
    FLASH->NSKEYR = UNLOCK_KEY1;
    FLASH->NSKEYR = UNLOCK_KEY2;
  }
}

static void flash_wait(void)
{
  while(READ_BIT(FLASH->NSSR, FLASH_SR_BSY) != 0u)
  {
  }
}

static void flash_clear_flags(void)
{
  FLASH->NSCCR = FLASH_CCR_CLR_EOP | FLASH_ERRORS;
}

static uint32_t flash_errors(void)
{
  return FLASH->NSSR & FLASH_ERRORS;
}

static uint32_t erase_bank2_sector(uint32_t sector)
{
  flash_wait();
  flash_clear_flags();
  MODIFY_REG(
      FLASH->NSCR, FLASH_CR_SNB | FLASH_CR_BKSEL,
      (sector << FLASH_CR_SNB_Pos) | FLASH_CR_BKSEL | FLASH_CR_SER);
  SET_BIT(FLASH->NSCR, FLASH_CR_START);
  flash_wait();
  CLEAR_BIT(FLASH->NSCR, FLASH_CR_SER);
  erase_end_flag = READ_BIT(FLASH->NSSR, FLASH_SR_EOP);
  return flash_errors();
}

static uint32_t program_quad_word(uint32_t address, uint32_t const data[4])
{
  flash_wait();
  flash_clear_flags();
  SET_BIT(FLASH->NSCR, FLASH_CR_PG);
  uint32_t volatile* const target = (uint32_t volatile*)address;
  for(uint32_t word = 0; word < 4u; ++word)
  {
    target[word] = data[word];
  }
  flash_wait();
  CLEAR_BIT(FLASH->NSCR, FLASH_CR_PG);
  return flash_errors();
}

static uint32_t store_without_pg(uint32_t address)
{
  flash_clear_flags();
  *(uint32_t volatile*)address = 0u;
  return flash_errors();
}

int main(void)
{
  SystemClock_Config();
  flash_unlock();
  erase_status = erase_bank2_sector(DATA_SECTOR);
  sector_erased();
  program_status = program_quad_word(DATA_ADDRESS, record);
  record_programmed();
  stray_status = store_without_pg(DATA_ADDRESS + 16u);
  SET_BIT(FLASH->NSCR, FLASH_CR_LOCK);
  flash_done();
  signature.flash_kb = LL_GetFlashSize();
  signature.package  = LL_GetPackageType();
  signature.uid[0]   = LL_GetUID_Word0();
  signature.uid[1]   = LL_GetUID_Word1();
  signature.uid[2]   = LL_GetUID_Word2();
  signature_read();
  for(;;)
  {
    __WFI();
  }
}
