#include "stm32u5xx_hal.h"

#include <stdint.h>

#define IWDG_TIMEOUT_MS 500u
#define REFRESH_MS      100u

IWDG_HandleTypeDef hiwdg;
volatile uint32_t  refresh_enabled = 1;
volatile uint32_t  reset_by_watchdog;

void SysTick_Handler(void);

void SysTick_Handler(void)
{
  HAL_IncTick();
}

__attribute__((noipa)) void reset_cause_recorded(void)
{}

static void record_reset_cause(void)
{
  reset_by_watchdog = __HAL_RCC_GET_FLAG(RCC_FLAG_IWDGRST) ? 1u : 0u;
  __HAL_RCC_CLEAR_RESET_FLAGS();
  reset_cause_recorded();
}

static void MX_IWDG_Init(void)
{
  hiwdg.Instance       = IWDG;
  hiwdg.Init.Prescaler = IWDG_PRESCALER_32;
  hiwdg.Init.Window    = IWDG_WINDOW_DISABLE;
  hiwdg.Init.Reload    = IWDG_TIMEOUT_MS * (LSI_VALUE / 32u) / 1000u;
  hiwdg.Init.EWI       = 0;
  if(HAL_IWDG_Init(&hiwdg) != HAL_OK)
  {
    __disable_irq();
    for(;;)
    {
    }
  }
}

int main(void)
{
  HAL_Init();
  record_reset_cause();
  MX_IWDG_Init();
  for(;;)
  {
    if(refresh_enabled != 0)
    {
      HAL_IWDG_Refresh(&hiwdg);
    }
    HAL_Delay(REFRESH_MS);
  }
}
