#include "periph_test.h"
#include "stm32u5xx_ll_bus.h"
#include "stm32u5xx_ll_pwr.h"
#include "stm32u5xx_ll_rcc.h"
#include "stm32u5xx_ll_rtc.h"

static volatile uint32_t wakeups;
static volatile uint32_t alarms;

void RTC_IRQHandler(void);
void idle(void);
void calendar(void);
void alarm(void);
void backup(void);
void clock_only(void);

void idle(void)
{
  for(;;)
  {
    __WFI();
  }
}

void RTC_IRQHandler(void)
{
  if(LL_RTC_IsActiveFlag_WUT(RTC))
  {
    LL_RTC_ClearFlag_WUT(RTC);
    ++wakeups;
  }
  if(LL_RTC_IsActiveFlag_ALRA(RTC))
  {
    LL_RTC_ClearFlag_ALRA(RTC);
    LOG(RTC->TR);
    ++alarms;
  }
}

static void start_clock(void)
{
  LL_AHB3_GRP1_EnableClock(LL_AHB3_GRP1_PERIPH_PWR);
  LL_PWR_EnableBkUpAccess();
  LL_RCC_LSE_Enable();
  while(!LL_RCC_LSE_IsReady())
  {
  }
  LL_RCC_SetRTCClockSource(LL_RCC_RTC_CLKSOURCE_LSE);
  LL_RCC_EnableRTC();
  LL_APB3_GRP1_EnableClock(LL_APB3_GRP1_PERIPH_RTCAPB);
}

static void start_rtc(void)
{
  start_clock();
  LL_RTC_InitTypeDef init = {
      .HourFormat      = LL_RTC_HOURFORMAT_24HOUR,
      .AsynchPrescaler = 127,
      .SynchPrescaler  = 255,
  };
  LL_RTC_Init(RTC, &init);
  LL_RTC_TimeTypeDef time = {
      .TimeFormat = LL_RTC_TIME_FORMAT_AM_OR_24,
      .Hours      = 23,
      .Minutes    = 59,
      .Seconds    = 58,
  };
  LL_RTC_TIME_Init(RTC, LL_RTC_FORMAT_BIN, &time);
  LL_RTC_DateTypeDef date = {
      .WeekDay = LL_RTC_WEEKDAY_SATURDAY,
      .Month   = LL_RTC_MONTH_FEBRUARY,
      .Day     = 28,
      .Year    = 26,
  };
  LL_RTC_DATE_Init(RTC, LL_RTC_FORMAT_BIN, &date);
}

static void wakeup_every_second(void)
{
  LL_RTC_DisableWriteProtection(RTC);
  LL_RTC_WAKEUP_Disable(RTC);
  while(!LL_RTC_IsActiveFlag_WUTW(RTC))
  {
  }
  LL_RTC_WAKEUP_SetClock(RTC, LL_RTC_WAKEUPCLOCK_CKSPRE);
  LL_RTC_WAKEUP_SetAutoReload(RTC, 0);
  LL_RTC_ClearFlag_WUT(RTC);
  LL_RTC_EnableIT_WUT(RTC);
  LL_RTC_WAKEUP_Enable(RTC);
  LL_RTC_EnableWriteProtection(RTC);
  NVIC_EnableIRQ(RTC_IRQn);
}

void calendar(void)
{
  start_rtc();
  wakeup_every_second();
  while(wakeups < 3)
  {
    __WFI();
  }
  LOG(RTC->TR);
  LOG(RTC->DR);
  while(wakeups < 5)
  {
    __WFI();
  }
  LOG(wakeups);
  DONE();
}

void alarm(void)
{
  start_rtc();
  LL_RTC_AlarmTypeDef config = {
      .AlarmTime =
          {
                      .TimeFormat = LL_RTC_TIME_FORMAT_AM_OR_24,
                      .Hours      = 0,
                      .Minutes    = 0,
                      .Seconds    = 1,
                      },
      .AlarmMask           = LL_RTC_ALMA_MASK_DATEWEEKDAY,
      .AlarmDateWeekDaySel = LL_RTC_ALMA_DATEWEEKDAYSEL_DATE,
      .AlarmDateWeekDay    = 1,
  };
  LL_RTC_ALMA_Init(RTC, LL_RTC_FORMAT_BIN, &config);
  LL_RTC_DisableWriteProtection(RTC);
  LL_RTC_ClearFlag_ALRA(RTC);
  LL_RTC_EnableIT_ALRA(RTC);
  LL_RTC_ALMA_Enable(RTC);
  LL_RTC_EnableWriteProtection(RTC);
  wakeup_every_second();
  while(wakeups < 5)
  {
    __WFI();
  }
  LOG(alarms);
  DONE();
}

void backup(void)
{
  start_rtc();
  LL_RTC_BKP_SetRegister(RTC, LL_RTC_BKP_DR0, 0xCAFE1234u);
  wakeup_every_second();
  idle();
}

void clock_only(void)
{
  start_clock();
  LOG(LL_RTC_IsActiveFlag_INITS(RTC));
  DONE();
}

int main(void)
{
  idle();
}
