# RTC calendar, wakeup timer, alarm and backup register, HAL

An STM32L476 application runs the RTC on the 32.768 kHz LSE with ST's HAL RTC driver.
`HAL_RTC_MspInit` sets PWR_CR1.DBP, starts the LSE, selects it as RTCCLK and sets RTCEN in RCC_BDCR.
On its first boot the firmware sets the calendar to 23:59:50 on Thursday 31 December 2026. On every
boot it counts the boot in RTC_BKP0R, arms alarm A at 00:00:05 any day, and runs the wakeup timer on
ck_spre at 1 Hz. The wakeup and alarm callbacks read the calendar back. The main loop sleeps in WFI.

It ports `stm32l152/rtc` to the STM32L4 RTC, RM0351 38, with its backup registers inside the
RTC and its interrupts on EXTI lines 18 and 20, RM0351 Table 62. The tests cross midnight
into 2027, count the wakeups and the alarm, and reset the machine: a software reset keeps the
backup domain and the running calendar, a power-on reset clears it. While RTC_ISR.INITS is set
the firmware skips `HAL_RTC_Init`, whose init mode would stop the calendar, as the RM0351 38.3.7
note suggests. `MachineDescription(strict=True)` makes any register access the models do not
serve fail the run.

## Run

From the repository root:

```sh
uv run pytest -q examples/stm32l476/rtc
```

## Read first

- `test_rtc_l476.py`:
  - the first boot: the calendar set, the LSE, RTCSEL, RTCEN and DBP;
  - EXTI_IMR1 and EXTI_RTSR1 lines 18 and 20 set by the HAL;
  - the calendar read in the wakeup callback across midnight, and RTC_DR.WDU Friday;
  - intercepts on `RTC_WKUP_IRQHandler` and `RTC_Alarm_IRQHandler`, one second apart and the
    alarm after the 15th wakeup;
  - `machine.reset(ResetKind.SOFTWARE)` and `ResetKind.POWER_ON`, the boot count in RTC_BKP0R
    and the calendar after each.
- `conftest.py`: the boot stops at `rtc_ready`, after the calendar, the alarm and the wakeup
  timer are set.
- `firmware/main.c`: the MSP init, the first-boot calendar and the callbacks.

## Rebuild the firmware

With arm-none-eabi-gcc on PATH:

```sh
uv run pcons -B build -j4
cp build/example-firmware-m4f/examples/stm32l476/rtc/rtc_calendar.elf examples/stm32l476/rtc/prebuilt/
```

The build links `../STM32L476XX_FLASH.ld` for 256 KB flash and the 96 KB SRAM1.
