#include "periph_test.h"
#include "stm32l4xx_hal.h"

static RNG_HandleTypeDef rng;

void idle(void);
void SysTick_Handler(void);

void idle(void)
{
  for(;;)
  {
    __WFI();
  }
}

void SysTick_Handler(void)
{
  HAL_IncTick();
}

int main(void)
{
  HAL_Init();
  __HAL_RCC_RNG_CONFIG(RCC_RNGCLKSOURCE_MSI);
  __HAL_RCC_RNG_CLK_ENABLE();
  rng.Instance = RNG;
  if(HAL_RNG_Init(&rng) != HAL_OK)
  {
    FAIL(1);
  }
  for(uint32_t word = 0; word < 4u; ++word)
  {
    uint32_t value = 0;
    if(HAL_RNG_GenerateRandomNumber(&rng, &value) != HAL_OK)
    {
      FAIL(2);
    }
    LOG(value);
  }
  LOG(HAL_RNG_GetState(&rng));
  LOG(RNG->CR);
  DONE();
}
