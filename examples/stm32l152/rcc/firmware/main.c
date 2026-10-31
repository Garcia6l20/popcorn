#include "main.h"

void SysTick_Handler(void);

__attribute__((noipa)) void hse_ready(void)
{}

__attribute__((noipa)) void pll_ready(void)
{}

__attribute__((noipa)) void clock_configured(void)
{}

__attribute__((noinline)) void Error_Handler(void)
{
  __disable_irq();
  for(;;)
  {
  }
}

static void SystemClock_Config(void)
{
  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);
  while(__HAL_PWR_GET_FLAG(PWR_FLAG_VOS))
  {
  }

  RCC_OscInitTypeDef hse = {
      .OscillatorType = RCC_OSCILLATORTYPE_HSE,
      .HSEState       = RCC_HSE_BYPASS,
      .PLL            = {.PLLState = RCC_PLL_NONE},
  };
  if(HAL_RCC_OscConfig(&hse) != HAL_OK)
  {
    Error_Handler();
  }
  hse_ready();

  RCC_OscInitTypeDef pll = {
      .OscillatorType = RCC_OSCILLATORTYPE_NONE,
      .PLL =
          {
                .PLLState  = RCC_PLL_ON,
                .PLLSource = RCC_PLLSOURCE_HSE,
                .PLLMUL    = RCC_PLL_MUL12,
                .PLLDIV    = RCC_PLL_DIV3,
                },
  };
  if(HAL_RCC_OscConfig(&pll) != HAL_OK)
  {
    Error_Handler();
  }
  pll_ready();

  RCC_ClkInitTypeDef clocks = {
      .ClockType =
          RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2,
      .SYSCLKSource   = RCC_SYSCLKSOURCE_PLLCLK,
      .AHBCLKDivider  = RCC_SYSCLK_DIV1,
      .APB1CLKDivider = RCC_HCLK_DIV1,
      .APB2CLKDivider = RCC_HCLK_DIV1,
  };
  if(HAL_RCC_ClockConfig(&clocks, FLASH_LATENCY_1) != HAL_OK)
  {
    Error_Handler();
  }
  clock_configured();
}

void SysTick_Handler(void)
{
  HAL_IncTick();
}

int main(void)
{
  HAL_Init();
  SystemClock_Config();
  for(;;)
  {
    __WFI();
  }
}
