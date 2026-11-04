#include "main.h"

#include "stm32l4xx.h"
#include "stm32l4xx_ll_bus.h"
#include "stm32l4xx_ll_rcc.h"
#include "stm32l4xx_ll_rng.h"

#define RNG_CR_CED_L4 (1u << 5)

struct rng_key    rng_key;
volatile uint32_t rng_use_msi             = 1u;
volatile uint32_t rng_detect_clock_errors = 1u;

__attribute__((noipa)) void rng_done(void)
{}

static uint32_t wait_ready(void)
{
  for(uint32_t polls = 0; polls < RNG_MAX_POLLS; ++polls)
  {
    if(LL_RNG_IsActiveFlag_DRDY(RNG))
    {
      return 1u;
    }
  }
  return 0u;
}

static uint32_t error_status(void)
{
  if(LL_RNG_IsActiveFlag_SECS(RNG) || LL_RNG_IsActiveFlag_SEIS(RNG))
  {
    return RNG_STATUS_SEED_ERROR;
  }
  if(LL_RNG_IsActiveFlag_CECS(RNG) || LL_RNG_IsActiveFlag_CEIS(RNG))
  {
    return RNG_STATUS_CLOCK_ERROR;
  }
  return RNG_STATUS_OK;
}

static uint32_t generate_key(void)
{
  uint32_t previous = 0;
  for(uint32_t word = 0; word < RNG_KEY_WORDS; ++word)
  {
    uint32_t const ready  = wait_ready();
    uint32_t const status = error_status();
    if(status != RNG_STATUS_OK)
    {
      return status;
    }
    if(!ready)
    {
      return RNG_STATUS_TIMEOUT;
    }
    uint32_t const value = LL_RNG_ReadRandData32(RNG);
    if(value == 0u)
    {
      return RNG_STATUS_SEED_ERROR;
    }
    if(word != 0u && value == previous)
    {
      return RNG_STATUS_REPEATED;
    }
    rng_key.key[word] = value;
    rng_key.words     = word + 1u;
    previous          = value;
  }
  return RNG_STATUS_OK;
}

int main(void)
{
  if(rng_use_msi != 0u)
  {
    LL_RCC_SetRNGClockSource(LL_RCC_RNG_CLKSOURCE_MSI);
  }
  LL_AHB2_GRP1_EnableClock(LL_AHB2_GRP1_PERIPH_RNG);
  if(rng_detect_clock_errors == 0u)
  {
    SET_BIT(RNG->CR, RNG_CR_CED_L4);
  }
  LL_RNG_Enable(RNG);
  rng_key.status = generate_key();
  LL_RNG_Disable(RNG);
  rng_key.done = 1u;
  rng_done();
  for(;;)
  {
    __WFI();
  }
}
