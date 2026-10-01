#include "main.h"

#include "stm32u5xx.h"
#include "stm32u5xx_ll_bus.h"
#include "stm32u5xx_ll_icache.h"
#include "stm32u5xx_ll_pwr.h"
#include "stm32u5xx_ll_rcc.h"
#include "stm32u5xx_ll_system.h"

#define UNLOCK_KEY1  0x45670123u
#define UNLOCK_KEY2  0xCDEF89ABu
#define DATA_PAGE    63u
#define DATA_ADDRESS 0x080FE000u
#define FLASH_ERRORS   \
  (FLASH_NSSR_PROGERR  \
   | FLASH_NSSR_WRPERR \
   | FLASH_NSSR_PGAERR \
   | FLASH_NSSR_SIZERR \
   | FLASH_NSSR_PGSERR \
   | FLASH_NSSR_OPTWERR)

volatile uint32_t erase_status;
volatile uint32_t program_status;
volatile uint32_t stray_status;

static uint32_t const record[4] = {0xC0FFEE00u, 0x12345678u, 0x9ABCDEF0u, 0x0BADF00Du};

__attribute__((noipa)) void clock_configured(void)
{}

__attribute__((noipa)) void page_erased(void)
{}

__attribute__((noipa)) void record_programmed(void)
{}

__attribute__((noipa)) void flash_done(void)
{}

__attribute__((noipa)) void set_latency(uint32_t wait_states)
{
  LL_FLASH_SetLatency(wait_states);
}

static void SystemClock_Config(void)
{
  LL_RCC_SetPll1EPodPrescaler(LL_RCC_PLL1MBOOST_DIV_1);
  LL_RCC_PLL1_SetMainSource(LL_RCC_PLL1SOURCE_MSIS);
  LL_AHB3_GRP1_EnableClock(LL_AHB3_GRP1_PERIPH_PWR);
  LL_PWR_SetRegulVoltageScaling(LL_PWR_REGU_VOLTAGE_SCALE1);
  while(LL_PWR_IsActiveFlag_VOS() == 0)
  {
  }
  LL_PWR_EnableEPODBooster();
  while(LL_PWR_IsActiveFlag_BOOST() == 0)
  {
  }
  set_latency(LL_FLASH_LATENCY_4);
  while(LL_FLASH_GetLatency() != LL_FLASH_LATENCY_4)
  {
  }

  LL_RCC_PLL1_ConfigDomain_SYS(LL_RCC_PLL1SOURCE_MSIS, 1, 80, 2);
  LL_RCC_PLL1_SetVCOInputRange(LL_RCC_PLLINPUTRANGE_4_8);
  LL_RCC_PLL1_EnableDomain_SYS();
  LL_RCC_PLL1_Enable();
  while(LL_RCC_PLL1_IsReady() == 0)
  {
  }

  LL_RCC_SetAHBPrescaler(LL_RCC_SYSCLK_DIV_1);
  LL_RCC_SetAPB1Prescaler(LL_RCC_APB1_DIV_1);
  LL_RCC_SetAPB2Prescaler(LL_RCC_APB2_DIV_1);
  LL_RCC_SetAPB3Prescaler(LL_RCC_APB3_DIV_1);
  LL_RCC_SetSysClkSource(LL_RCC_SYS_CLKSOURCE_PLL1);
  while(LL_RCC_GetSysClkSource() != LL_RCC_SYS_CLKSOURCE_STATUS_PLL1)
  {
  }
  SystemCoreClockUpdate();
  LL_ICACHE_Enable();
  clock_configured();
}

static void flash_unlock(void)
{
  if(READ_BIT(FLASH->NSCR, FLASH_NSCR_LOCK) != 0u)
  {
    FLASH->NSKEYR = UNLOCK_KEY1;
    FLASH->NSKEYR = UNLOCK_KEY2;
  }
}

static void flash_wait(void)
{
  while(READ_BIT(FLASH->NSSR, FLASH_NSSR_BSY) != 0u)
  {
  }
}

static void flash_clear_flags(void)
{
  FLASH->NSSR = FLASH_NSSR_EOP | FLASH_ERRORS;
}

static uint32_t flash_errors(void)
{
  return FLASH->NSSR & FLASH_ERRORS;
}

static uint32_t erase_bank2_page(uint32_t page)
{
  flash_wait();
  flash_clear_flags();
  MODIFY_REG(
      FLASH->NSCR, FLASH_NSCR_PNB | FLASH_NSCR_BKER,
      (page << FLASH_NSCR_PNB_Pos) | FLASH_NSCR_BKER | FLASH_NSCR_PER);
  SET_BIT(FLASH->NSCR, FLASH_NSCR_STRT);
  flash_wait();
  CLEAR_BIT(FLASH->NSCR, FLASH_NSCR_PER);
  return flash_errors();
}

static uint32_t program_quad_word(uint32_t address, uint32_t const data[4])
{
  flash_wait();
  flash_clear_flags();
  SET_BIT(FLASH->NSCR, FLASH_NSCR_PG);
  uint32_t volatile* const target = (uint32_t volatile*)address;
  for(uint32_t word = 0; word < 4u; ++word)
  {
    target[word] = data[word];
  }
  flash_wait();
  CLEAR_BIT(FLASH->NSCR, FLASH_NSCR_PG);
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
  erase_status = erase_bank2_page(DATA_PAGE);
  page_erased();
  program_status = program_quad_word(DATA_ADDRESS, record);
  record_programmed();
  stray_status = store_without_pg(DATA_ADDRESS + 16u);
  SET_BIT(FLASH->NSCR, FLASH_NSCR_LOCK);
  flash_done();
  for(;;)
  {
    __WFI();
  }
}
