"""The independent watchdog from HAL: refreshed every 100 ms, a reset 500 ms after the last one,
and a refresh above the window.

The IWDG counts the 32 kHz LSI of the STM32L4, RM0351 6.2.7, divided by 32, a 1 ms tick. The
firmware asks for 500 ms and the HAL loads a reload of 500, so the counter reaches 0 after
500 ms. The firmware reads the window from `iwdg_window`, 0xFFF by default, which leaves the
window open. A refresh while the counter is above IWDG_WINR is a reset, RM0351 36.3.2.

A watchdog reset request stops the run with `StopReason.RESET_REQUEST`, and `stop.detail`
names the requester. The test resets the machine with the matching `ResetKind`, as the chip
would, and the firmware reads IWDGRSTF back from RCC_CSR at boot.
"""

import pytest
from popcorn import Call, Machine, ResetKind, Return, StopReason
from popcorn.units import VirtualTime, ms, s

pytestmark = pytest.mark.popcorn(budget=20_000_000)

RELOAD = 500
TIMEOUT = ms(500)
REFRESH_PERIOD = ms(100)
COUNTER_TICK = ms(1)


def refreshes(machine: Machine) -> list[VirtualTime]:
    seen: list[VirtualTime] = []

    def record(call: Call) -> Return:
        seen.append(machine.time)
        return call.proceed()

    machine.intercept("HAL_IWDG_Refresh", record)
    return seen


async def test_a_power_on_boot_is_not_a_watchdog_reset(iwdg: Machine) -> None:
    stop = await iwdg.run(until="reset_cause_recorded")
    assert stop.reason is StopReason.ADDRESS, stop
    assert iwdg.var("reset_by_watchdog").value == 0


async def test_refreshing_keeps_the_watchdog_quiet(iwdg: Machine) -> None:
    seen = refreshes(iwdg)
    stop = await iwdg.run(for_=s(2))
    assert stop.reason is StopReason.TIME, stop
    assert len(seen) == 19
    assert iwdg.periph.IWDG.WINR.WIN == 0xFFF


async def test_the_watchdog_resets_500_ms_after_the_last_refresh(iwdg: Machine) -> None:
    seen = refreshes(iwdg)
    await iwdg.run(for_=ms(250))
    iwdg.var("refresh_enabled").value = 0

    stop = await iwdg.run(for_=s(1))
    assert stop.reason is StopReason.RESET_REQUEST, stop
    kind = ResetKind[stop.detail.upper()]
    assert kind is ResetKind.WATCHDOG
    assert iwdg.periph.IWDG.RLR.RL == RELOAD
    assert abs((stop.time - seen[-1]).ps - TIMEOUT.ps) <= COUNTER_TICK.ps

    iwdg.reset(kind)
    stop = await iwdg.run(until="reset_cause_recorded")
    assert stop.reason is StopReason.ADDRESS, stop
    assert iwdg.var("reset_by_watchdog").value == 1


async def test_the_next_run_applies_the_watchdog_reset_the_stop_reported(iwdg: Machine) -> None:
    await iwdg.run(for_=ms(250))
    iwdg.var("refresh_enabled").value = 0
    stop = await iwdg.run(for_=s(1))
    assert stop.reason is StopReason.RESET_REQUEST, stop
    assert stop.detail == "watchdog"
    resets = stop.resets

    stop = await iwdg.run(until="reset_cause_recorded")
    assert stop.reason is StopReason.ADDRESS, stop
    assert stop.resets == resets + 1
    assert iwdg.var("reset_by_watchdog").value == 1


async def test_refreshes_below_the_window_keep_the_watchdog_quiet(iwdg: Machine) -> None:
    iwdg.var("iwdg_window").value = 450
    seen = refreshes(iwdg)
    stop = await iwdg.run(for_=s(2))
    assert stop.reason is StopReason.TIME, stop
    assert len(seen) == 19
    assert iwdg.periph.IWDG.WINR.WIN == 450


async def test_a_refresh_above_the_window_resets_at_the_refresh(iwdg: Machine) -> None:
    iwdg.var("iwdg_window").value = 300
    stop = await iwdg.run(until="iwdg_started")
    assert stop.reason is StopReason.ADDRESS, stop
    started = iwdg.time
    seen = refreshes(iwdg)

    stop = await iwdg.run(for_=s(1))
    assert stop.reason is StopReason.RESET_REQUEST, stop
    assert stop.detail == "watchdog"
    assert iwdg.periph.IWDG.WINR.WIN == 300
    assert len(seen) == 1
    assert abs((seen[0] - started).ps - REFRESH_PERIOD.ps) <= COUNTER_TICK.ps
    assert (stop.time - seen[0]).ps < COUNTER_TICK.ps

    iwdg.reset(ResetKind.WATCHDOG)
    stop = await iwdg.run(until="reset_cause_recorded")
    assert stop.reason is StopReason.ADDRESS, stop
    assert iwdg.var("reset_by_watchdog").value == 1
