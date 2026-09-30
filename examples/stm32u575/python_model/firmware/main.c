#include "main.h"

RNG_HandleTypeDef hrng;
struct rng_result rng_result;

static volatile bool ready;

__attribute__((noinline)) void Error_Handler(void)
{
  __disable_irq();
  for(;;)
  {
  }
}

__attribute__((noipa)) void rng_done(void)
{}

void HAL_RNG_ReadyDataCallback(RNG_HandleTypeDef* handle, uint32_t random32bit)
{
  (void)handle;
  rng_result.words[rng_result.count++] = random32bit;
  ready                                = true;
}

static void MX_HSI48_Init(void)
{
  RCC_OscInitTypeDef oscillator = {
      .OscillatorType = RCC_OSCILLATORTYPE_HSI48,
      .HSI48State     = RCC_HSI48_ON,
      .PLL            = {.PLLState = RCC_PLL_NONE},
  };
  if(HAL_RCC_OscConfig(&oscillator) != HAL_OK)
  {
    Error_Handler();
  }
}

static void MX_RNG_Init(void)
{
  hrng.Instance                 = RNG;
  hrng.Init.ClockErrorDetection = RNG_CED_ENABLE;
  if(HAL_RNG_Init(&hrng) != HAL_OK)
  {
    Error_Handler();
  }
}

int main(void)
{
  HAL_Init();
  MX_HSI48_Init();
  MX_RNG_Init();
  while(rng_result.count < RANDOM_WORDS)
  {
    ready = false;
    if(HAL_RNG_GenerateRandomNumber_IT(&hrng) != HAL_OK)
    {
      Error_Handler();
    }
    __disable_irq();
    while(!ready)
    {
      __WFI();
      __enable_irq();
      __disable_irq();
    }
    __enable_irq();
  }
  rng_done();
  for(;;)
  {
    __WFI();
  }
}
