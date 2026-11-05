#include "stm32l4xx_hal.h"

#include <stdint.h>

#define BOOT_REGISTER RTC_BKP_DR0

RTC_HandleTypeDef hrtc;

volatile uint32_t boot_count;
volatile uint32_t calendar_set;
volatile uint32_t wakeups;
volatile uint32_t alarms;
volatile uint32_t seconds_of_day;
volatile uint32_t date_bcd;
volatile uint32_t alarm_seconds_of_day;

void SysTick_Handler(void);
void RTC_WKUP_IRQHandler(void);
void RTC_Alarm_IRQHandler(void);

void SysTick_Handler(void)
{
  HAL_IncTick();
}

void RTC_WKUP_IRQHandler(void)
{
  HAL_RTCEx_WakeUpTimerIRQHandler(&hrtc);
}

void RTC_Alarm_IRQHandler(void)
{
  HAL_RTC_AlarmIRQHandler(&hrtc);
}

__attribute__((noinline)) static void Error_Handler(void)
{
  __disable_irq();
  for(;;)
  {
  }
}

__attribute__((noipa)) void rtc_ready(void)
{}

static uint32_t read_seconds_of_day(void)
{
  RTC_TimeTypeDef time = {0};
  RTC_DateTypeDef date = {0};
  HAL_RTC_GetTime(&hrtc, &time, RTC_FORMAT_BIN);
  HAL_RTC_GetDate(&hrtc, &date, RTC_FORMAT_BIN);
  date_bcd = (uint32_t)date.Year << 16 | (uint32_t)date.Month << 8 | date.Date;
  return (uint32_t)time.Hours * 3600u + (uint32_t)time.Minutes * 60u + time.Seconds;
}

void HAL_RTCEx_WakeUpTimerEventCallback(RTC_HandleTypeDef* handle)
{
  (void)handle;
  ++wakeups;
  seconds_of_day = read_seconds_of_day();
}

void HAL_RTC_AlarmAEventCallback(RTC_HandleTypeDef* handle)
{
  (void)handle;
  ++alarms;
  alarm_seconds_of_day = read_seconds_of_day();
}

void HAL_RTC_MspInit(RTC_HandleTypeDef* handle)
{
  (void)handle;
  __HAL_RCC_PWR_CLK_ENABLE();
  HAL_PWR_EnableBkUpAccess();
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
      .PeriphClockSelection = RCC_PERIPHCLK_RTC,
      .RTCClockSelection    = RCC_RTCCLKSOURCE_LSE,
  };
  if(HAL_RCCEx_PeriphCLKConfig(&clock) != HAL_OK)
  {
    Error_Handler();
  }
  __HAL_RCC_RTC_ENABLE();
  HAL_NVIC_SetPriority(RTC_WKUP_IRQn, 2, 0);
  HAL_NVIC_EnableIRQ(RTC_WKUP_IRQn);
  HAL_NVIC_SetPriority(RTC_Alarm_IRQn, 2, 0);
  HAL_NVIC_EnableIRQ(RTC_Alarm_IRQn);
}

static void MX_RTC_Init(void)
{
  hrtc.Instance            = RTC;
  hrtc.Init.HourFormat     = RTC_HOURFORMAT_24;
  hrtc.Init.AsynchPrediv   = 127;
  hrtc.Init.SynchPrediv    = 255;
  hrtc.Init.OutPut         = RTC_OUTPUT_DISABLE;
  hrtc.Init.OutPutRemap    = RTC_OUTPUT_REMAP_NONE;
  hrtc.Init.OutPutPolarity = RTC_OUTPUT_POLARITY_HIGH;
  hrtc.Init.OutPutType     = RTC_OUTPUT_TYPE_OPENDRAIN;
  if(READ_BIT(RCC->BDCR, RCC_BDCR_RTCEN) != 0u && READ_BIT(RTC->ISR, RTC_ISR_INITS) != 0u)
  {
    HAL_RTC_MspInit(&hrtc);
    hrtc.State = HAL_RTC_STATE_READY;
    if(HAL_RTC_WaitForSynchro(&hrtc) != HAL_OK)
    {
      Error_Handler();
    }
    return;
  }
  if(HAL_RTC_Init(&hrtc) != HAL_OK)
  {
    Error_Handler();
  }
}

static void set_calendar(void)
{
  RTC_TimeTypeDef time = {
      .Hours          = 23,
      .Minutes        = 59,
      .Seconds        = 50,
      .DayLightSaving = RTC_DAYLIGHTSAVING_NONE,
      .StoreOperation = RTC_STOREOPERATION_RESET,
  };
  RTC_DateTypeDef date = {
      .WeekDay = RTC_WEEKDAY_THURSDAY,
      .Month   = RTC_MONTH_DECEMBER,
      .Date    = 31,
      .Year    = 26,
  };
  if(HAL_RTC_SetTime(&hrtc, &time, RTC_FORMAT_BIN) != HAL_OK
     || HAL_RTC_SetDate(&hrtc, &date, RTC_FORMAT_BIN) != HAL_OK)
  {
    Error_Handler();
  }
  calendar_set = 1;
}

static void arm_interrupts(void)
{
  RTC_AlarmTypeDef alarm = {
      .AlarmTime           = {.Hours = 0, .Minutes = 0, .Seconds = 5},
      .AlarmMask           = RTC_ALARMMASK_DATEWEEKDAY,
      .AlarmSubSecondMask  = RTC_ALARMSUBSECONDMASK_ALL,
      .AlarmDateWeekDaySel = RTC_ALARMDATEWEEKDAYSEL_DATE,
      .AlarmDateWeekDay    = 1,
      .Alarm               = RTC_ALARM_A,
  };
  if(HAL_RTC_SetAlarm_IT(&hrtc, &alarm, RTC_FORMAT_BIN) != HAL_OK
     || HAL_RTCEx_SetWakeUpTimer_IT(&hrtc, 0, RTC_WAKEUPCLOCK_CK_SPRE_16BITS) != HAL_OK)
  {
    Error_Handler();
  }
}

int main(void)
{
  HAL_Init();
  MX_RTC_Init();
  boot_count = HAL_RTCEx_BKUPRead(&hrtc, BOOT_REGISTER) + 1u;
  HAL_RTCEx_BKUPWrite(&hrtc, BOOT_REGISTER, boot_count);
  if(boot_count == 1u)
  {
    set_calendar();
  }
  arm_interrupts();
  seconds_of_day = read_seconds_of_day();
  rtc_ready();
  for(;;)
  {
    __WFI();
  }
}
