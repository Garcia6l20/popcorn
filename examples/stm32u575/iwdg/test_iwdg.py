"""The independent watchdog from HAL: refreshed every 100 ms, a reset 500 ms after the last one.

A watchdog reset request stops the run with `StopReason.RESET_REQUEST`, and `stop.detail`
names the requester. The test resets the machine with the matching `ResetKind`, as the chip
would, and the firmware reads IWDGRSTF back from RCC_CSR at boot.
"""

import pytest
from popcorn import Call, Machine, ResetKind, Return, StopReason
from popcorn.units import VirtualTime, ms, s, us

pytestmark = pytest.mark.popcorn(budget=20_000_000)

TIMEOUT = ms(500)
LSI_TICK = us(31.25)


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
    assert len(seen) == 20


async def test_the_watchdog_resets_500_ms_after_the_last_refresh(iwdg: Machine) -> None:
    seen = refreshes(iwdg)
    await iwdg.run(for_=ms(250))
    iwdg.var("refresh_enabled").value = 0

    stop = await iwdg.run(for_=s(1))
    assert stop.reason is StopReason.RESET_REQUEST, stop
    kind = ResetKind[stop.detail.upper()]
    assert kind is ResetKind.WATCHDOG
    assert abs((stop.time - seen[-1]).ps - TIMEOUT.ps) <= LSI_TICK.ps

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
