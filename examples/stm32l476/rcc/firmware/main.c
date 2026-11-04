#include "main.h"

void SysTick_Handler(void);

__attribute__((noipa)) void range_1_set(void)
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
  if(HAL_PWREx_ControlVoltageScaling(PWR_REGULATOR_VOLTAGE_SCALE1) != HAL_OK)
  {
    Error_Handler();
  }
  range_1_set();

  RCC_OscInitTypeDef pll = {
      .OscillatorType      = RCC_OSCILLATORTYPE_MSI,
      .MSIState            = RCC_MSI_ON,
      .MSICalibrationValue = RCC_MSICALIBRATION_DEFAULT,
      .MSIClockRange       = RCC_MSIRANGE_6,
      .PLL =
          {
                .PLLState  = RCC_PLL_ON,
                .PLLSource = RCC_PLLSOURCE_MSI,
                .PLLM      = 1,
                .PLLN      = 40,
                .PLLP      = RCC_PLLP_DIV7,
                .PLLQ      = RCC_PLLQ_DIV2,
                .PLLR      = RCC_PLLR_DIV2,
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
  if(HAL_RCC_ClockConfig(&clocks, FLASH_LATENCY_4) != HAL_OK)
  {
    Error_Handler();
  }
  HAL_RCC_MCOConfig(RCC_MCO1, RCC_MCO1SOURCE_SYSCLK, RCC_MCODIV_16);
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
