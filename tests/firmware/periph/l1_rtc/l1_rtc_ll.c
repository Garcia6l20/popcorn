#include "periph_test.h"
#include "stm32l1xx.h"
#include "stm32l1xx_ll_bus.h"
#include "stm32l1xx_ll_pwr.h"
#include "stm32l1xx_ll_rcc.h"
#include "stm32l1xx_ll_rtc.h"

void idle(void);
void calendar(void);
void read_calendar(void);
void read_after_reset(void);
void backup_write(void);
void backup_read(void);
void backup_reset(void);

void idle(void)
{
  for(;;)
  {
    __WFI();
  }
}

static void start_clock(void)
{
  LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_PWR);
  LL_PWR_EnableBkUpAccess();
  LL_RCC_LSE_Enable();
  while(!LL_RCC_LSE_IsReady())
  {
  }
  LL_RCC_SetRTCClockSource(LL_RCC_RTC_CLKSOURCE_LSE);
  LL_RCC_EnableRTC();
}

static void log_calendar(void)
{
#if defined(RTC_SSR_SS)
  LOG(RTC->SSR);
#endif
  LOG(RTC->TR);
  LOG(RTC->DR);
}

void calendar(void)
{
  start_clock();
  LL_RTC_InitTypeDef init = {
      .HourFormat      = LL_RTC_HOURFORMAT_24HOUR,
      .AsynchPrescaler = 127,
      .SynchPrescaler  = 255,
  };
  LOG(LL_RTC_Init(RTC, &init));
  LL_RTC_TimeTypeDef time = {
      .TimeFormat = LL_RTC_TIME_FORMAT_AM_OR_24,
      .Hours      = 23,
      .Minutes    = 59,
      .Seconds    = 58,
  };
  LOG(LL_RTC_TIME_Init(RTC, LL_RTC_FORMAT_BIN, &time));
  LL_RTC_DateTypeDef date = {
      .WeekDay = LL_RTC_WEEKDAY_SATURDAY,
      .Month   = LL_RTC_MONTH_FEBRUARY,
      .Day     = 28,
      .Year    = 26,
  };
  LOG(LL_RTC_DATE_Init(RTC, LL_RTC_FORMAT_BIN, &date));
  LOG(LL_RTC_IsActiveFlag_INITS(RTC));
  log_calendar();
  idle();
}

void read_calendar(void)
{
  while(!LL_RTC_IsActiveFlag_RS(RTC))
  {
  }
  log_calendar();
  DONE();
}

void read_after_reset(void)
{
  LOG(LL_RTC_IsActiveFlag_RS(RTC));
  log_calendar();
  read_calendar();
}

void backup_write(void)
{
  start_clock();
  LL_RTC_BAK_SetRegister(RTC, LL_RTC_BKP_DR0, 0xCAFE1234u);
  LL_RTC_BAK_SetRegister(RTC, LL_RTC_BKP_DR19, 0x5EED0019u);
  idle();
}

void backup_read(void)
{
  LOG(LL_RTC_BAK_GetRegister(RTC, LL_RTC_BKP_DR0));
  LOG(LL_RTC_BAK_GetRegister(RTC, LL_RTC_BKP_DR19));
  DONE();
}

void backup_reset(void)
{
  LL_RCC_ForceBackupDomainReset();
  LL_RCC_ReleaseBackupDomainReset();
  backup_read();
}

int main(void)
{
  idle();
}
