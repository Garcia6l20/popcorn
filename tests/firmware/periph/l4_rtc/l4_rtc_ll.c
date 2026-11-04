#include "periph_test.h"
#include "stm32l4xx.h"
#include "stm32l4xx_ll_bus.h"
#include "stm32l4xx_ll_exti.h"
#include "stm32l4xx_ll_pwr.h"
#include "stm32l4xx_ll_rcc.h"
#include "stm32l4xx_ll_rtc.h"

static volatile uint32_t wakeups;
static volatile uint32_t alarms;
static volatile uint32_t stamps;

void RTC_Alarm_IRQHandler(void);
void RTC_WKUP_IRQHandler(void);
void TAMP_STAMP_IRQHandler(void);
void timestamp(void);
void idle(void);
void alarm(void);
void wakeup(void);
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

void RTC_Alarm_IRQHandler(void)
{
  if(LL_RTC_IsActiveFlag_ALRA(RTC))
  {
    LL_RTC_ClearFlag_ALRA(RTC);
    LOG(RTC->TR);
    LOG(RTC->DR);
    ++alarms;
  }
  LL_EXTI_ClearFlag_0_31(LL_EXTI_LINE_18);
}

void RTC_WKUP_IRQHandler(void)
{
  if(LL_RTC_IsActiveFlag_WUT(RTC))
  {
    LL_RTC_ClearFlag_WUT(RTC);
    ++wakeups;
  }
  LL_EXTI_ClearFlag_0_31(LL_EXTI_LINE_20);
}

void TAMP_STAMP_IRQHandler(void)
{
  if(LL_RTC_IsActiveFlag_TS(RTC))
  {
    LOG(RTC->TSTR);
    LOG(RTC->TSDR);
    LL_RTC_ClearFlag_TS(RTC);
    ++stamps;
  }
  LL_EXTI_ClearFlag_0_31(LL_EXTI_LINE_19);
}

static void log_calendar(void)
{
  LOG(RTC->SSR);
  LOG(RTC->TR);
  LOG(RTC->DR);
}

static void start_calendar(void)
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
}

void calendar(void)
{
  start_calendar();
  LOG(LL_RTC_IsActiveFlag_INITS(RTC));
  log_calendar();
  idle();
}

static void wakeup_every_second(void)
{
  LL_EXTI_EnableIT_0_31(LL_EXTI_LINE_20);
  LL_EXTI_EnableRisingTrig_0_31(LL_EXTI_LINE_20);
  NVIC_EnableIRQ(RTC_WKUP_IRQn);
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
}

void wakeup(void)
{
  start_calendar();
  wakeup_every_second();
  while(wakeups < 3)
  {
    __WFI();
  }
  log_calendar();
  LOG(wakeups);
  DONE();
}

void alarm(void)
{
  start_calendar();
  LL_EXTI_EnableIT_0_31(LL_EXTI_LINE_18);
  LL_EXTI_EnableRisingTrig_0_31(LL_EXTI_LINE_18);
  NVIC_EnableIRQ(RTC_Alarm_IRQn);
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
  LOG(LL_RTC_ALMA_Init(RTC, LL_RTC_FORMAT_BIN, &config));
  LL_RTC_DisableWriteProtection(RTC);
  LL_RTC_ClearFlag_ALRA(RTC);
  LL_RTC_EnableIT_ALRA(RTC);
  LL_RTC_ALMA_Enable(RTC);
  LL_RTC_EnableWriteProtection(RTC);
  while(alarms < 1)
  {
    __WFI();
  }
  LOG(alarms);
  DONE();
}

void timestamp(void)
{
  start_calendar();
  LL_EXTI_EnableIT_0_31(LL_EXTI_LINE_19);
  LL_EXTI_EnableRisingTrig_0_31(LL_EXTI_LINE_19);
  NVIC_EnableIRQ(TAMP_STAMP_IRQn);
  LL_RTC_DisableWriteProtection(RTC);
  LL_RTC_TS_SetActiveEdge(RTC, LL_RTC_TIMESTAMP_EDGE_RISING);
  LL_RTC_EnableIT_TS(RTC);
  LL_RTC_TS_Enable(RTC);
  LL_RTC_EnableWriteProtection(RTC);
  while(stamps < 1)
  {
    __WFI();
  }
  LOG(stamps);
  DONE();
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
  LL_RTC_BAK_SetRegister(RTC, LL_RTC_BKP_DR31, 0x5EED0031u);
  idle();
}

void backup_read(void)
{
  LOG(LL_RTC_BAK_GetRegister(RTC, LL_RTC_BKP_DR0));
  LOG(LL_RTC_BAK_GetRegister(RTC, LL_RTC_BKP_DR31));
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
