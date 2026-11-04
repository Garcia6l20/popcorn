#include "stm32l4xx_hal.h"

#include <stdint.h>

#define PERIOD_TICKS 4096u
#define PULSE_TICKS  2048u

LPTIM_HandleTypeDef hlptim1;

volatile uint32_t periods;
volatile uint32_t compares;

void SysTick_Handler(void);
void LPTIM1_IRQHandler(void);

void SysTick_Handler(void)
{
  HAL_IncTick();
}

void LPTIM1_IRQHandler(void)
{
  HAL_LPTIM_IRQHandler(&hlptim1);
}

__attribute__((noinline)) static void Error_Handler(void)
{
  __disable_irq();
  for(;;)
  {
  }
}

__attribute__((noipa)) void lptim_started(void)
{}

void HAL_LPTIM_CompareMatchCallback(LPTIM_HandleTypeDef* hlptim)
{
  (void)hlptim;
  ++compares;
}

void HAL_LPTIM_AutoReloadMatchCallback(LPTIM_HandleTypeDef* hlptim)
{
  (void)hlptim;
  ++periods;
  HAL_GPIO_TogglePin(GPIOA, GPIO_PIN_5);
}

void HAL_LPTIM_MspInit(LPTIM_HandleTypeDef* hlptim)
{
  if(hlptim->Instance != LPTIM1)
  {
    return;
  }
  RCC_OscInitTypeDef oscillators = {
      .OscillatorType = RCC_OSCILLATORTYPE_LSE,
      .LSEState       = RCC_LSE_ON,
      .PLL            = {.PLLState = RCC_PLL_NONE},
  };
  if(HAL_RCC_OscConfig(&oscillators) != HAL_OK)
  {
    Error_Handler();
  }
  RCC_PeriphCLKInitTypeDef clock = {
      .PeriphClockSelection = RCC_PERIPHCLK_LPTIM1,
      .Lptim1ClockSelection = RCC_LPTIM1CLKSOURCE_LSE,
  };
  if(HAL_RCCEx_PeriphCLKConfig(&clock) != HAL_OK)
  {
    Error_Handler();
  }
  __HAL_RCC_LPTIM1_CLK_ENABLE();
  HAL_NVIC_SetPriority(LPTIM1_IRQn, 2, 0);
  HAL_NVIC_EnableIRQ(LPTIM1_IRQn);
}

static void MX_GPIO_Init(void)
{
  __HAL_RCC_GPIOA_CLK_ENABLE();
  HAL_GPIO_WritePin(GPIOA, GPIO_PIN_5, GPIO_PIN_RESET);
  GPIO_InitTypeDef led = {
      .Pin   = GPIO_PIN_5,
      .Mode  = GPIO_MODE_OUTPUT_PP,
      .Pull  = GPIO_NOPULL,
      .Speed = GPIO_SPEED_FREQ_LOW,
  };
  HAL_GPIO_Init(GPIOA, &led);
}

static void MX_LPTIM1_Init(void)
{
  hlptim1.Instance             = LPTIM1;
  hlptim1.Init.Clock.Source    = LPTIM_CLOCKSOURCE_APBCLOCK_LPOSC;
  hlptim1.Init.Clock.Prescaler = LPTIM_PRESCALER_DIV1;
  hlptim1.Init.Trigger.Source  = LPTIM_TRIGSOURCE_SOFTWARE;
  hlptim1.Init.OutputPolarity  = LPTIM_OUTPUTPOLARITY_HIGH;
  hlptim1.Init.UpdateMode      = LPTIM_UPDATE_IMMEDIATE;
  hlptim1.Init.CounterSource   = LPTIM_COUNTERSOURCE_INTERNAL;
  hlptim1.Init.Input1Source    = LPTIM_INPUT1SOURCE_GPIO;
  hlptim1.Init.Input2Source    = LPTIM_INPUT2SOURCE_GPIO;
  if(HAL_LPTIM_Init(&hlptim1) != HAL_OK)
  {
    Error_Handler();
  }
}

int main(void)
{
  HAL_Init();
  MX_GPIO_Init();
  MX_LPTIM1_Init();
  if(HAL_LPTIM_PWM_Start_IT(&hlptim1, PERIOD_TICKS - 1u, PULSE_TICKS - 1u) != HAL_OK)
  {
    Error_Handler();
  }
  lptim_started();
  for(;;)
  {
    __WFI();
  }
}
