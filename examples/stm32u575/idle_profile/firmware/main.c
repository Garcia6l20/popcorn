#include "main.h"

#include "FreeRTOS.h"
#include "task.h"

#include "stm32u5xx_ll_bus.h"
#include "stm32u5xx_ll_tim.h"

#define TIMER_CLOCK_HZ     10000u
#define FLAG_HZ            4u
#define FLAG_DELAY_MS      240u
#define PULSE_MS           2u
#define STATUS_DELAY_MS    100u
#define TIMER_IRQ_PRIORITY 6u

volatile uint32_t timer_flag;
volatile uint32_t timer_ticks;
volatile uint32_t flags_seen;
volatile uint32_t pulses_seen;

void TIM2_IRQHandler(void);

void TIM2_IRQHandler(void)
{
  if(LL_TIM_IsActiveFlag_UPDATE(TIM2))
  {
    LL_TIM_ClearFlag_UPDATE(TIM2);
    ++timer_ticks;
    timer_flag = 1;
  }
}

static void SystemClock_Config(void)
{
  if(HAL_PWREx_ControlVoltageScaling(PWR_REGULATOR_VOLTAGE_SCALE1) != HAL_OK)
  {
    Error_Handler();
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
      .ClockType      = RCC_CLOCKTYPE_HCLK
                      | RCC_CLOCKTYPE_SYSCLK
                      | RCC_CLOCKTYPE_PCLK1
                      | RCC_CLOCKTYPE_PCLK2
                      | RCC_CLOCKTYPE_PCLK3,
      .SYSCLKSource   = RCC_SYSCLKSOURCE_HSI,
      .AHBCLKDivider  = RCC_SYSCLK_DIV1,
      .APB1CLKDivider = RCC_HCLK_DIV1,
      .APB2CLKDivider = RCC_HCLK_DIV1,
      .APB3CLKDivider = RCC_HCLK_DIV1,
  };
  if(HAL_RCC_ClockConfig(&clocks, FLASH_LATENCY_1) != HAL_OK)
  {
    Error_Handler();
  }
}

static void MX_TIM2_Init(void)
{
  LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_TIM2);
  LL_TIM_SetPrescaler(TIM2, __LL_TIM_CALC_PSC(SystemCoreClock, TIMER_CLOCK_HZ));
  LL_TIM_SetAutoReload(TIM2, TIMER_CLOCK_HZ / FLAG_HZ - 1u);
  LL_TIM_GenerateEvent_UPDATE(TIM2);
  LL_TIM_ClearFlag_UPDATE(TIM2);
  LL_TIM_EnableIT_UPDATE(TIM2);
  NVIC_SetPriority(TIM2_IRQn, TIMER_IRQ_PRIORITY);
  NVIC_EnableIRQ(TIM2_IRQn);
  LL_TIM_EnableCounter(TIM2);
}

static void MX_TIM16_Init(void)
{
  LL_APB2_GRP1_EnableClock(LL_APB2_GRP1_PERIPH_TIM16);
  LL_TIM_SetPrescaler(TIM16, __LL_TIM_CALC_PSC(SystemCoreClock, TIMER_CLOCK_HZ));
  LL_TIM_SetAutoReload(TIM16, TIMER_CLOCK_HZ / 1000u * PULSE_MS - 1u);
  LL_TIM_SetOnePulseMode(TIM16, LL_TIM_ONEPULSEMODE_SINGLE);
  LL_TIM_SetUpdateSource(TIM16, LL_TIM_UPDATESOURCE_COUNTER);
  LL_TIM_GenerateEvent_UPDATE(TIM16);
  LL_TIM_ClearFlag_UPDATE(TIM16);
}

static void flag_task(void* argument)
{
  (void)argument;
  for(;;)
  {
    vTaskDelay(pdMS_TO_TICKS(FLAG_DELAY_MS));
    while(timer_flag == 0)
    {
    }
    timer_flag = 0;
    ++flags_seen;
  }
}

static void status_task(void* argument)
{
  (void)argument;
  for(;;)
  {
    vTaskDelay(pdMS_TO_TICKS(STATUS_DELAY_MS));
    LL_TIM_EnableCounter(TIM16);
    while(!LL_TIM_IsActiveFlag_UPDATE(TIM16))
    {
    }
    LL_TIM_ClearFlag_UPDATE(TIM16);
    ++pulses_seen;
  }
}

#ifdef IDLE_WFI
void vApplicationIdleHook(void)
{
  __WFI();
}
#endif

__attribute__((noinline)) void vAssertCalled(char const* file, int line)
{
  (void)file;
  (void)line;
  taskDISABLE_INTERRUPTS();
  for(;;)
  {
  }
}

__attribute__((noinline)) void Error_Handler(void)
{
  __disable_irq();
  for(;;)
  {
  }
}

void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef* htim)
{
  if(htim->Instance == TIM6)
  {
    HAL_IncTick();
  }
}

int main(void)
{
  HAL_Init();
  SystemClock_Config();
  MX_TIM2_Init();
  MX_TIM16_Init();
  if(xTaskCreate(status_task, "status", configMINIMAL_STACK_SIZE, NULL, 2, NULL) != pdPASS
     || xTaskCreate(flag_task, "flag", configMINIMAL_STACK_SIZE, NULL, 1, NULL) != pdPASS)
  {
    Error_Handler();
  }
  vTaskStartScheduler();
  Error_Handler();
}
