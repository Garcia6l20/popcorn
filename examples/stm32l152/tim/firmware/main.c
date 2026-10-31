#include "stm32l1xx_hal.h"

#include <stdint.h>

#define TIMER_CLOCK_HZ 1000000u
#define TICK_HZ        1000u
#define PULSE_DELAY_MS 50u

TIM_HandleTypeDef htim2;
TIM_HandleTypeDef htim3;
TIM_HandleTypeDef htim6;

volatile uint32_t ticks;
volatile uint32_t pulses;
volatile uint32_t ticks_at_pulse;
volatile uint32_t captures;
volatile uint32_t last_capture;
volatile uint32_t capture_period;

void SysTick_Handler(void);
void TIM2_IRQHandler(void);
void TIM3_IRQHandler(void);
void TIM6_IRQHandler(void);

void SysTick_Handler(void)
{
  HAL_IncTick();
}

void TIM2_IRQHandler(void)
{
  HAL_TIM_IRQHandler(&htim2);
}

void TIM3_IRQHandler(void)
{
  HAL_TIM_IRQHandler(&htim3);
}

void TIM6_IRQHandler(void)
{
  HAL_TIM_IRQHandler(&htim6);
}

__attribute__((noinline)) static void Error_Handler(void)
{
  __disable_irq();
  for(;;)
  {
  }
}

__attribute__((noipa)) void timers_started(void)
{}

void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef* htim)
{
  if(htim->Instance == TIM2)
  {
    ++ticks;
    HAL_GPIO_TogglePin(GPIOA, GPIO_PIN_5);
  }
  else if(htim->Instance == TIM6)
  {
    ++pulses;
    ticks_at_pulse = ticks;
  }
}

void HAL_TIM_IC_CaptureCallback(TIM_HandleTypeDef* htim)
{
  if(htim->Instance != TIM3)
  {
    return;
  }
  uint32_t const now = HAL_TIM_ReadCapturedValue(htim, TIM_CHANNEL_1);
  if(captures != 0)
  {
    capture_period = (now - last_capture) & 0xFFFFu;
  }
  last_capture = now;
  ++captures;
}

void HAL_TIM_Base_MspInit(TIM_HandleTypeDef* htim)
{
  if(htim->Instance == TIM2)
  {
    __HAL_RCC_TIM2_CLK_ENABLE();
    HAL_NVIC_SetPriority(TIM2_IRQn, 2, 0);
    HAL_NVIC_EnableIRQ(TIM2_IRQn);
  }
  else if(htim->Instance == TIM6)
  {
    __HAL_RCC_TIM6_CLK_ENABLE();
    HAL_NVIC_SetPriority(TIM6_IRQn, 3, 0);
    HAL_NVIC_EnableIRQ(TIM6_IRQn);
  }
}

void HAL_TIM_IC_MspInit(TIM_HandleTypeDef* htim)
{
  if(htim->Instance != TIM3)
  {
    return;
  }
  __HAL_RCC_TIM3_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  GPIO_InitTypeDef pin = {
      .Pin       = GPIO_PIN_6,
      .Mode      = GPIO_MODE_AF_PP,
      .Pull      = GPIO_PULLDOWN,
      .Speed     = GPIO_SPEED_FREQ_LOW,
      .Alternate = GPIO_AF2_TIM3,
  };
  HAL_GPIO_Init(GPIOA, &pin);
  HAL_NVIC_SetPriority(TIM3_IRQn, 1, 0);
  HAL_NVIC_EnableIRQ(TIM3_IRQn);
}

static void SystemClock_Config(void)
{
  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);
  while(__HAL_PWR_GET_FLAG(PWR_FLAG_VOS))
  {
  }
  RCC_OscInitTypeDef oscillators = {
      .OscillatorType      = RCC_OSCILLATORTYPE_HSI,
      .HSIState            = RCC_HSI_ON,
      .HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT,
      .PLL                 = {.PLLState = RCC_PLL_NONE},
  };
  if(HAL_RCC_OscConfig(&oscillators) != HAL_OK)
  {
    Error_Handler();
  }
  RCC_ClkInitTypeDef clocks = {
      .ClockType =
          RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2,
      .SYSCLKSource   = RCC_SYSCLKSOURCE_HSI,
      .AHBCLKDivider  = RCC_SYSCLK_DIV1,
      .APB1CLKDivider = RCC_HCLK_DIV1,
      .APB2CLKDivider = RCC_HCLK_DIV1,
  };
  if(HAL_RCC_ClockConfig(&clocks, FLASH_LATENCY_1) != HAL_OK)
  {
    Error_Handler();
  }
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

static void base_init(TIM_HandleTypeDef* htim, TIM_TypeDef* instance, uint32_t hz, uint32_t period)
{
  htim->Instance               = instance;
  htim->Init.Prescaler         = SystemCoreClock / hz - 1u;
  htim->Init.CounterMode       = TIM_COUNTERMODE_UP;
  htim->Init.Period            = period - 1u;
  htim->Init.ClockDivision     = TIM_CLOCKDIVISION_DIV1;
  htim->Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
  if(HAL_TIM_Base_Init(htim) != HAL_OK)
  {
    Error_Handler();
  }
}

static void MX_TIM3_Init(void)
{
  htim3.Instance               = TIM3;
  htim3.Init.Prescaler         = SystemCoreClock / TIMER_CLOCK_HZ - 1u;
  htim3.Init.CounterMode       = TIM_COUNTERMODE_UP;
  htim3.Init.Period            = 0xFFFFu;
  htim3.Init.ClockDivision     = TIM_CLOCKDIVISION_DIV1;
  htim3.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
  if(HAL_TIM_IC_Init(&htim3) != HAL_OK)
  {
    Error_Handler();
  }
  TIM_IC_InitTypeDef capture = {
      .ICPolarity  = TIM_ICPOLARITY_RISING,
      .ICSelection = TIM_ICSELECTION_DIRECTTI,
      .ICPrescaler = TIM_ICPSC_DIV1,
      .ICFilter    = 0,
  };
  if(HAL_TIM_IC_ConfigChannel(&htim3, &capture, TIM_CHANNEL_1) != HAL_OK)
  {
    Error_Handler();
  }
}

int main(void)
{
  HAL_Init();
  SystemClock_Config();
  MX_GPIO_Init();
  base_init(&htim2, TIM2, TIMER_CLOCK_HZ, TIMER_CLOCK_HZ / TICK_HZ);
  base_init(&htim6, TIM6, TICK_HZ, PULSE_DELAY_MS);
  MX_TIM3_Init();
  SET_BIT(htim6.Instance->CR1, TIM_CR1_OPM);
  if(HAL_TIM_IC_Start_IT(&htim3, TIM_CHANNEL_1) != HAL_OK
     || HAL_TIM_Base_Start_IT(&htim2) != HAL_OK
     || HAL_TIM_Base_Start_IT(&htim6) != HAL_OK)
  {
    Error_Handler();
  }
  timers_started();
  for(;;)
  {
    __WFI();
  }
}
