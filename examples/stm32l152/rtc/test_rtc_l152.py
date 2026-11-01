"""The STM32L1 RTC on the 32.768 kHz LSE: a calendar across midnight and a new year, the wakeup
timer every second, alarm A, and a backup register across resets, HAL.

`HAL_RTC_MspInit` enables backup domain access through PWR_CR.DBP, starts the LSE, selects it as
RTCCLK and sets RTCEN. On its first boot the firmware sets the calendar to 23:59:50 on Thursday
31 December 2026. On every boot it counts the boot in RTC_BKP0R, arms alarm A at 00:00:05 any
day and the wakeup timer on ck_spre, 1 Hz, with HAL interrupts on EXTI lines 17 and 20. The
wakeup callback counts and reads the calendar into `seconds_of_day` and `date_bcd`, the alarm
callback into `alarm_seconds_of_day`.

A software reset keeps the backup domain: the calendar runs on, BKP0R holds the boot count, and
the firmware skips the calendar setup. It also skips `HAL_RTC_Init` while RTC_ISR.INITS is set,
as the RM0038 20.3.5 note suggests, since the init mode it enters stops the calendar, and waits
for RSF with `HAL_RTC_WaitForSynchro` before it reads the calendar, since TR and DR read their
reset values after a system reset until RSF sets, RM0038 20.3.6. A power-on
reset clears the backup domain, so the next boot sets the calendar again. The project sets
`MachineDescription(strict=True)`.
"""

import itertools
from collections.abc import Callable

import pytest
from popcorn import Call, Machine, ResetKind, Return, StopReason
from popcorn.units import VirtualTime, ms, s

pytestmark = pytest.mark.popcorn(budget=5_000_000)

SET_SECONDS = 23 * 3600 + 59 * 60 + 50
NEW_YEAR = 27 << 16 | 1 << 8 | 1
OLD_YEAR = 26 << 16 | 12 << 8 | 31
FRIDAY = 5
RTCSEL_LSE = 1
RSF_BIT = 5


def interrupts(machine: Machine) -> list[tuple[str, VirtualTime]]:
    seen: list[tuple[str, VirtualTime]] = []

    def recorder(name: str) -> Callable[[Call], Return]:
        def record(call: Call) -> Return:
            seen.append((name, machine.time))
            return call.proceed()

        return record

    machine.intercept("RTC_WKUP_IRQHandler", recorder("wakeup"))
    machine.intercept("RTC_Alarm_IRQHandler", recorder("alarm"))
    return seen


async def run_for(machine: Machine, duration: int) -> None:
    stop = await machine.run(for_=ms(duration))
    assert stop.reason is StopReason.TIME, stop


async def reboot(machine: Machine, kind: ResetKind) -> None:
    machine.reset(kind)
    stop = await machine.run(until="rtc_ready", for_=s(1))
    assert stop.reason is StopReason.ADDRESS, stop


def value(machine: Machine, name: str) -> int:
    read = machine.var(name).value
    assert isinstance(read, int)
    return read


async def test_the_first_boot_sets_the_calendar_on_the_lse(rtc: Machine) -> None:
    assert value(rtc, "calendar_set") == 1
    assert value(rtc, "boot_count") == 1
    assert value(rtc, "seconds_of_day") == SET_SECONDS
    assert value(rtc, "date_bcd") == OLD_YEAR
    rcc = rtc.periph.RCC
    assert (rcc.CSR.LSERDY, rcc.CSR.RTCSEL, rcc.CSR.RTCEN) == (1, RTCSEL_LSE, 1)
    assert rtc.periph.PWR.CR.DBP == 1


async def test_the_calendar_crosses_midnight_into_the_new_year(rtc: Machine) -> None:
    await run_for(rtc, 11_000)
    assert value(rtc, "date_bcd") == NEW_YEAR
    assert value(rtc, "seconds_of_day") == 1
    assert rtc.periph.RTC.DR.WDU == FRIDAY


async def test_the_wakeup_timer_fires_every_second(rtc: Machine) -> None:
    seen = interrupts(rtc)
    await run_for(rtc, 5_500)
    times = [at for name, at in seen if name == "wakeup"]
    assert len(times) == 5
    assert all(later - earlier == s(1) for earlier, later in itertools.pairwise(times))
    assert value(rtc, "wakeups") == 5
    assert value(rtc, "seconds_of_day") == SET_SECONDS + 5


async def test_alarm_a_fires_once_at_five_past_midnight(rtc: Machine) -> None:
    seen = interrupts(rtc)
    await run_for(rtc, 16_500)
    assert [name for name, _ in seen].index("alarm") == 15
    assert value(rtc, "alarms") == 1
    assert value(rtc, "alarm_seconds_of_day") == 5


async def test_a_software_reset_keeps_the_calendar_and_the_boot_count(rtc: Machine) -> None:
    await run_for(rtc, 3_000)
    inits: list[VirtualTime] = []

    def record(call: Call) -> Return:
        inits.append(rtc.time)
        return call.proceed()

    synchronised: list[VirtualTime] = []
    shadows_ready: list[int] = []

    def synchro(call: Call) -> Return:
        synchronised.append(rtc.time)
        return call.proceed()

    def get_time(call: Call) -> Return:
        shadows_ready.append(rtc.periph.RTC.ISR.read() >> RSF_BIT & 1)
        return call.proceed()

    rtc.intercept("HAL_RTC_Init", record)
    rtc.intercept("HAL_RTC_WaitForSynchro", synchro)
    rtc.intercept("HAL_RTC_GetTime", get_time)
    await reboot(rtc, ResetKind.SOFTWARE)
    assert inits == []
    assert len(synchronised) == 1
    assert shadows_ready == [1]
    assert value(rtc, "boot_count") == 2
    assert value(rtc, "calendar_set") == 0
    assert value(rtc, "seconds_of_day") == SET_SECONDS + 3
    assert rtc.periph.RTC.BKP0R.value == 2
    await run_for(rtc, 2_500)
    assert value(rtc, "wakeups") == 2


async def test_a_power_on_reset_clears_the_backup_domain(rtc: Machine) -> None:
    await run_for(rtc, 3_000)
    await reboot(rtc, ResetKind.POWER_ON)
    assert value(rtc, "boot_count") == 1
    assert value(rtc, "calendar_set") == 1
    assert value(rtc, "seconds_of_day") == SET_SECONDS
