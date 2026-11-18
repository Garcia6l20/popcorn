"""Where a FreeRTOS firmware spends an idle second, with and without WFI in its idle hook.

Both fixtures run one source: `idle_wfi` built with `IDLE_WFI`, whose idle hook sleeps in WFI,
and `idle_spin` without it, whose idle task spins. Each test profiles one virtual second from
`main` with `Machine.profile` and reads the report: the sleep share, the loop runs of the two
polling tasks and the register they poll.
"""

import pytest
from popcorn import Machine, StopReason
from popcorn.profiling import LoopRow, Report
from popcorn.units import s

pytestmark = pytest.mark.popcorn(budget=20_000_000)

ASLEEP_SHARE_WITH_WFI = 0.9
"""Measured 0.9314: the idle task sleeps from tick to tick, the two polls spin about 6 %."""
IDLE_SPIN_SHARE = 0.9
"""Measured 0.9328 for the idle task's loop without the hook."""


async def profile_one_second(machine: Machine, *analyses: str) -> Report:
    with machine.profile(*analyses) as profile:
        stop = await machine.run(for_=s(1))
    assert stop.reason is StopReason.TIME, stop
    return profile.report()


def loop_in(report: Report, function: str) -> LoopRow:
    assert report.loops is not None
    (row,) = (row for row in report.loops.loops if row.location.startswith(function))
    return row


async def test_the_idle_hook_sleeps_in_wfi_between_ticks(idle_wfi: Machine) -> None:
    report = await profile_one_second(idle_wfi, "sleep")

    sleep = report.sleep
    assert sleep is not None
    assert sleep.share > ASLEEP_SHARE_WITH_WFI
    assert sleep.reasons["wfi"] == sleep.count > 0
    assert {row.name for row in sleep.wakes} == {"SysTick", "TIM6"}


async def test_without_the_hook_the_idle_task_spins(idle_spin: Machine) -> None:
    report = await profile_one_second(idle_spin, "sleep", "loops")

    assert report.sleep is not None
    assert report.sleep.count == 0
    assert report.sleep.time_ps == 0
    assert report.loops is not None
    idle = loop_in(report, "prvCheckTasksWaitingTermination")
    assert idle.share > IDLE_SPIN_SHARE
    assert set(idle.enders) == {"exception"}


async def test_a_ram_flag_poll_ends_on_an_exception(idle_wfi: Machine) -> None:
    report = await profile_one_second(idle_wfi, "loops")

    flag = loop_in(report, "flag_task")
    assert flag.enders["exception"] > 0
    assert "progress" not in flag.enders
    assert idle_wfi.var("flags_seen").value == idle_wfi.var("timer_ticks").value > 0


async def test_a_status_register_poll_ends_when_the_read_value_changes(idle_wfi: Machine) -> None:
    report = await profile_one_second(idle_wfi, "loops", "peripherals")

    status = loop_in(report, "status_task")
    pulses = idle_wfi.var("pulses_seen").value
    assert status.enders["progress"] == pulses > 0
    assert report.peripherals is not None
    (sr,) = (row for row in report.peripherals.cpu if row.name == "TIM16.SR")
    assert sr.reads > status.passes
