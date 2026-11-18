import io
from pathlib import Path

import popcorn
import popcorn.pytest
from popcorn.profiling import Report, diff
from popcorn.units import ms, s

PREBUILT = Path(__file__).parents[3] / "examples/stm32u575/idle_profile/prebuilt"


def idle(name: str) -> popcorn.MachineDescription:
    return popcorn.MachineDescription(
        name=name,
        part="STM32U575QGIx",
        images=[popcorn.Elf(PREBUILT / f"{name}.elf")],
        boot=popcorn.Boot(until="main", budget=1_000_000),
    )


IDLE_SPIN = idle("idle_spin")
IDLE_WFI = idle("idle_wfi")
TICKS = popcorn.MachineDescription(
    part="STM32U575QGIx",
    images=[popcorn.Elf(PREBUILT.parents[1] / "tim/prebuilt/tim_ticks.elf")],
    boot=popcorn.Boot(until="main", budget=1_000_000),
)


@popcorn.pytest.machine
def idle_spin() -> popcorn.MachineDescription:
    return IDLE_SPIN


@popcorn.pytest.machine
def idle_wfi() -> popcorn.MachineDescription:
    return IDLE_WFI


@popcorn.pytest.machine
def ticks() -> popcorn.MachineDescription:
    return TICKS


async def one_second(machine: popcorn.Machine) -> Report:
    with machine.profile() as profile:
        await machine.run(for_=s(1))
    return profile.report()


async def test_the_spinning_idle_task_takes_the_hotspots(idle_spin: popcorn.Machine) -> None:
    # --8<-- [start:profile]
    with idle_spin.profile() as profile:
        await idle_spin.run(for_=s(1))
    report = profile.report()
    print(report.text())
    # --8<-- [end:profile]

    # --8<-- [start:read]
    assert report.hotspots is not None
    top = report.hotspots.functions[0]
    assert top.name == "prvCheckTasksWaitingTermination"
    assert report.sleep is not None
    assert report.sleep.count == 0
    assert report.loops is not None
    assert report.loops.loops[0].location == "prvCheckTasksWaitingTermination"
    # --8<-- [end:read]
    assert top.self > report.executed // 2


async def test_the_idle_hook_turns_the_spin_into_sleep(
    idle_spin: popcorn.Machine, idle_wfi: popcorn.Machine
) -> None:
    spin = await one_second(idle_spin)
    # --8<-- [start:wfi]
    with idle_wfi.profile() as profile:
        await idle_wfi.run(for_=s(1))
    wfi = profile.report()
    assert wfi.sleep is not None
    assert wfi.sleep.share > 0.9
    assert wfi.sleep.reasons["wfi"] == wfi.sleep.count
    assert {row.name for row in wfi.sleep.wakes} == {"SysTick", "TIM6"}
    # --8<-- [end:wfi]

    # --8<-- [start:diff]
    changes = diff(spin, wfi)
    assert changes["summary"]["report"]["executed"] == wfi.executed - spin.executed
    assert changes["sleep"]["sleep"]["count"] == wfi.sleep.count
    # --8<-- [end:diff]
    assert "prvCheckTasksWaitingTermination" in changes["functions"]


async def test_the_polls_show_as_loop_runs(idle_wfi: popcorn.Machine) -> None:
    # --8<-- [start:loops]
    with idle_wfi.profile("loops", "peripherals") as profile:
        await idle_wfi.run(for_=s(1))
    report = profile.report()
    assert report.loops is not None
    flag, status = (
        next(row for row in report.loops.loops if row.location.startswith(function))
        for function in ("flag_task", "status_task")
    )
    assert "progress" not in flag.enders
    assert status.enders["progress"] == idle_wfi.var("pulses_seen").value
    assert report.peripherals is not None
    assert report.peripherals.cpu[0].name == "TIM16.SR"
    # --8<-- [end:loops]


async def test_a_report_round_trips_through_json_and_writes_folded_stacks(
    idle_wfi: popcorn.Machine, tmp_path: Path
) -> None:
    report = await one_second(idle_wfi)
    # --8<-- [start:export]
    (tmp_path / "idle.json").write_text(report.to_json())
    again = Report.from_json((tmp_path / "idle.json").read_text())
    assert again == report
    report.write_folded(tmp_path / "idle.folded")
    # --8<-- [end:export]
    folded = io.StringIO()
    assert report.write_folded(folded) > 0
    assert (tmp_path / "idle.folded").read_text() == folded.getvalue()


async def test_a_profile_reports_the_sleeps_and_the_timer_interrupt(ticks: popcorn.Machine) -> None:
    with ticks.profile("sleep", "irq") as profile:
        await ticks.run(for_=ms(100))
    report = profile.report()

    assert report.sleep is not None
    assert report.sleep.share > 0.99
    assert report.irq is not None
    assert report.irq.exceptions[0].name == "TIM2"
