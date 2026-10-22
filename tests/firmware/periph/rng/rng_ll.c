#include "periph_test.h"
#include "stm32u5xx_ll_bus.h"
#include "stm32u5xx_ll_pwr.h"
#include "stm32u5xx_ll_rcc.h"
#include "stm32u5xx_ll_rng.h"

#define RNG_WORDS     16u
#define RNG_MAX_POLLS 2000u
#define RNG_NIST_CR   0x00F00D00u
#define RNG_NIST_HTCR 0x0000A2B0u
#define RNG_NIST_NSCR 0x00017CBBu
#define RNG_RESET_CR  0x00800D00u

#define KEYGEN_OK        0u
#define KEYGEN_SEED      1u
#define KEYGEN_CLOCK     2u
#define KEYGEN_REPEATED  3u
#define KEYGEN_NOT_READY 4u

volatile uint32_t keygen_status;
volatile uint32_t keygen_done;
uint32_t          keygen_key[RNG_WORDS];

void idle(void);
void enable_read(void);
void enable_read_idle(void);
void nist_init(void);
void locked(void);
void no_kernel_clock(void);
void hsi16_source(void);
void pll_160(void);
void checkpoint(void) __attribute__((noinline));
void seed_recover_condrst(void);
void seed_recover_auto(void);
void clock_error(void);
void clock_error_ced_off(void);
void keygen(void);
void deinit(void);

void checkpoint(void)
{
  __asm__ volatile("" ::: "memory");
}

void idle(void)
{
  for(;;)
  {
    __WFI();
  }
}

static void hsi48_on(void)
{
  LL_RCC_HSI48_Enable();
  while(!LL_RCC_HSI48_IsReady())
  {
  }
}

static uint32_t poll_ready(void)
{
  uint32_t polls = 0;
  while(!LL_RNG_IsActiveFlag_DRDY(RNG) && polls < RNG_MAX_POLLS)
  {
    ++polls;
  }
  return polls;
}

static uint32_t poll_set(uint32_t mask)
{
  uint32_t polls = 0;
  while((RNG->SR & mask) == 0u && polls < RNG_MAX_POLLS)
  {
    ++polls;
  }
  return polls;
}

static uint32_t poll_clear(uint32_t mask)
{
  uint32_t polls = 0;
  while((RNG->SR & mask) != 0u && polls < RNG_MAX_POLLS)
  {
    ++polls;
  }
  return polls;
}

static uint32_t poll_condrst(void)
{
  uint32_t polls = 0;
  while(LL_RNG_IsEnabledCondReset(RNG) && polls < RNG_MAX_POLLS)
  {
    ++polls;
  }
  return polls;
}

static void read_four(void)
{
  for(uint32_t word = 0; word < 4u; ++word)
  {
    LOG(poll_ready());
    LOG(LL_RNG_ReadRandData32(RNG));
  }
}

static void configure(uint32_t control)
{
  RNG->CR = control | RNG_CR_CONDRST;
  RNG->CR = control;
  LOG(poll_condrst());
}

static void read_words(void)
{
  LL_AHB2_GRP1_EnableClock(LL_AHB2_GRP1_PERIPH_RNG);
  LL_RNG_Enable(RNG);
  for(uint32_t word = 0; word < RNG_WORDS; ++word)
  {
    uint32_t const polls = poll_ready();
    LOG(RNG->SR);
    LOG(polls);
    LOG(LL_RNG_ReadRandData32(RNG));
  }
  LL_RNG_Disable(RNG);
}

void enable_read(void)
{
  hsi48_on();
  read_words();
  LL_RCC_HSI48_Disable();
  LOG(RNG->CR);
  DONE();
}

void enable_read_idle(void)
{
  hsi48_on();
  read_words();
  LL_RCC_HSI48_Disable();
  LOG(RNG->CR);
  idle();
}

void nist_init(void)
{
  hsi48_on();
  LL_AHB2_GRP1_EnableClock(LL_AHB2_GRP1_PERIPH_RNG);
  RNG->CR        = RNG_NIST_CR | RNG_CR_CONDRST;
  RNG->HTCR      = RNG_NIST_HTCR;
  RNG->NSCR      = RNG_NIST_NSCR;
  RNG->CR        = RNG_NIST_CR;
  uint32_t polls = 0;
  while(LL_RNG_IsEnabledCondReset(RNG))
  {
    ++polls;
  }
  LOG(RNG->CR);
  LOG(RNG->HTCR);
  LOG(RNG->NSCR);
  LOG(polls);
  LL_RNG_Enable(RNG);
  LOG(poll_ready());
  LOG(RNG->SR);
  DONE();
}

void locked(void)
{
  hsi48_on();
  LL_AHB2_GRP1_EnableClock(LL_AHB2_GRP1_PERIPH_RNG);
  LL_RNG_ConfigLock(RNG);
  RNG->CR   = RNG_NIST_CR | RNG_CR_CONDRST;
  RNG->HTCR = RNG_NIST_HTCR;
  RNG->NSCR = RNG_NIST_NSCR;
  LOG(RNG->CR);
  LOG(RNG->HTCR);
  LOG(RNG->NSCR);
  DONE();
}

void no_kernel_clock(void)
{
  LL_AHB2_GRP1_EnableClock(LL_AHB2_GRP1_PERIPH_RNG);
  LL_RNG_Enable(RNG);
  LOG(poll_ready());
  LOG(RNG->SR);
  LOG(LL_RNG_ReadRandData32(RNG));
  DONE();
}

void hsi16_source(void)
{
  LL_RCC_HSI_Enable();
  while(!LL_RCC_HSI_IsReady())
  {
  }
  LL_RCC_SetRNGClockSource(LL_RCC_RNG_CLKSOURCE_HSI);
  read_words();
  LOG(LL_RCC_HSI48_IsReady());
  DONE();
}

void pll_160(void)
{
  LL_AHB3_GRP1_EnableClock(LL_AHB3_GRP1_PERIPH_PWR);
  LL_PWR_SetRegulVoltageScaling(LL_PWR_REGU_VOLTAGE_SCALE1);
  while(!LL_PWR_IsActiveFlag_VOS())
  {
  }
  LL_PWR_EnableEPODBooster();
  while(!LL_PWR_IsActiveFlag_BOOST())
  {
  }
  FLASH->ACR = (FLASH->ACR & ~FLASH_ACR_LATENCY) | 4u;
  LL_RCC_PLL1_ConfigDomain_SYS(LL_RCC_PLL1SOURCE_MSIS, 1, 80, 2);
  LL_RCC_PLL1_SetVCOInputRange(LL_RCC_PLLINPUTRANGE_4_8);
  LL_RCC_PLL1_EnableDomain_SYS();
  LL_RCC_PLL1_Enable();
  while(!LL_RCC_PLL1_IsReady())
  {
  }
  LL_RCC_SetSysClkSource(LL_RCC_SYS_CLKSOURCE_PLL1);
  while(LL_RCC_GetSysClkSource() != LL_RCC_SYS_CLKSOURCE_STATUS_PLL1)
  {
  }
  hsi48_on();
  LL_AHB2_GRP1_EnableClock(LL_AHB2_GRP1_PERIPH_RNG);
  DONE();
}

void seed_recover_condrst(void)
{
  hsi48_on();
  LL_AHB2_GRP1_EnableClock(LL_AHB2_GRP1_PERIPH_RNG);
  configure(RNG_RESET_CR | RNG_CR_ARDIS);
  LL_RNG_Enable(RNG);
  LOG(poll_set(RNG_SR_SEIS));
  LOG(RNG->SR);
  checkpoint();
  LL_RNG_EnableCondReset(RNG);
  LL_RNG_DisableCondReset(RNG);
  LOG(poll_condrst());
  LL_RNG_ClearFlag_SEIS(RNG);
  LOG(RNG->SR);
  LOG(poll_clear(RNG_SR_SECS));
  LOG(RNG->SR);
  read_four();
  LOG(RNG->SR);
  DONE();
}

void seed_recover_auto(void)
{
  hsi48_on();
  LL_AHB2_GRP1_EnableClock(LL_AHB2_GRP1_PERIPH_RNG);
  LL_RNG_Enable(RNG);
  LOG(poll_set(RNG_SR_SEIS));
  LOG(RNG->SR);
  checkpoint();
  LL_RNG_ClearFlag_SEIS(RNG);
  LOG(poll_clear(RNG_SR_SECS));
  LOG(RNG->SR);
  read_four();
  LOG(RNG->SR);
  DONE();
}

void clock_error(void)
{
  hsi48_on();
  LL_AHB2_GRP1_EnableClock(LL_AHB2_GRP1_PERIPH_RNG);
  LL_RNG_Enable(RNG);
  LOG(poll_ready());
  LOG(LL_RNG_ReadRandData32(RNG));
  LL_RCC_HSI48_Disable();
  LOG(poll_set(RNG_SR_CECS));
  LOG(RNG->SR);
  hsi48_on();
  LOG(poll_clear(RNG_SR_CECS));
  LOG(RNG->SR);
  RNG->SR = 0u;
  LOG(RNG->SR);
  DONE();
}

void clock_error_ced_off(void)
{
  hsi48_on();
  LL_AHB2_GRP1_EnableClock(LL_AHB2_GRP1_PERIPH_RNG);
  configure(RNG_RESET_CR | RNG_CR_CED);
  LL_RNG_Enable(RNG);
  LOG(poll_ready());
  LOG(LL_RNG_ReadRandData32(RNG));
  LL_RCC_HSI48_Disable();
  LOG(poll_set(RNG_SR_CECS));
  LOG(RNG->SR);
  DONE();
}

static uint32_t generate_key(void)
{
  uint32_t previous = 0;
  for(uint32_t word = 0; word < RNG_WORDS; ++word)
  {
    if(poll_ready() == RNG_MAX_POLLS)
    {
      return (RNG->SR & RNG_SR_SECS) != 0u ? KEYGEN_SEED : KEYGEN_NOT_READY;
    }
    uint32_t const status = RNG->SR;
    if((status & (RNG_SR_SECS | RNG_SR_SEIS)) != 0u)
    {
      return KEYGEN_SEED;
    }
    if((status & (RNG_SR_CECS | RNG_SR_CEIS)) != 0u)
    {
      return KEYGEN_CLOCK;
    }
    uint32_t const value = LL_RNG_ReadRandData32(RNG);
    if(value == 0u)
    {
      return KEYGEN_SEED;
    }
    if(word != 0u && value == previous)
    {
      return KEYGEN_REPEATED;
    }
    keygen_key[word] = value;
    previous         = value;
  }
  return KEYGEN_OK;
}

void keygen(void)
{
  hsi48_on();
  LL_AHB2_GRP1_EnableClock(LL_AHB2_GRP1_PERIPH_RNG);
  LL_RNG_Enable(RNG);
  keygen_status = generate_key();
  LL_RNG_Disable(RNG);
  LL_RCC_HSI48_Disable();
  LOG(keygen_status);
  keygen_done = 1u;
  idle();
}

static void nist_configure(void)
{
  RNG->CR   = RNG_NIST_CR | RNG_CR_CONDRST;
  RNG->HTCR = RNG_NIST_HTCR;
  RNG->NSCR = RNG_NIST_NSCR;
  RNG->CR   = RNG_NIST_CR;
  LOG(poll_condrst());
}

static void log_registers(void)
{
  LOG(RNG->CR);
  LOG(RNG->SR);
  LOG(RNG->NSCR);
  LOG(RNG->HTCR);
}

void deinit(void)
{
  hsi48_on();
  LL_AHB2_GRP1_EnableClock(LL_AHB2_GRP1_PERIPH_RNG);
  nist_configure();
  LL_RNG_Enable(RNG);
  LL_RNG_ConfigLock(RNG);
  read_four();
  log_registers();
  LL_RNG_DeInit(RNG);
  log_registers();
  LOG(RNG->DR);
  nist_configure();
  LL_RNG_Enable(RNG);
  LL_RNG_ConfigLock(RNG);
  read_four();
  log_registers();
  idle();
}

int main(void)
{
  idle();
}
