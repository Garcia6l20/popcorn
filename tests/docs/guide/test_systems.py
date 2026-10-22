from pathlib import Path
from typing import cast

import popcorn
import popcorn.pytest
import pytest
from popcorn import LineState, StopReason, System
from popcorn.units import VirtualTime, ms, us

PREBUILT = Path(__file__).parents[3] / "examples/stm32u575/linked_uart/prebuilt"

# --8<-- [start:describe]
CONTROLLER = popcorn.MachineDescription(
    part="STM32U575QGIx",
    images=[popcorn.Elf(PREBUILT / "linked_controller.elf")],
    boot=popcorn.Boot(until="links_ready", budget=2_000_000),
)
RESPONDER = popcorn.MachineDescription(
    part="STM32U575QGIx",
    images=[popcorn.Elf(PREBUILT / "linked_responder.elf")],
    boot=popcorn.Boot(until="links_ready", budget=2_000_000),
)
SYSTEM = popcorn.SystemDescription(
    members={"controller": CONTROLLER, "responder1": RESPONDER, "responder2": RESPONDER},
    links=[
        popcorn.UartLink("controller.UART4", "responder1.USART3", latency=us(20), baud=460_800),
        popcorn.UartLink("controller.USART3", "responder2.USART3", latency=us(20), baud=460_800),
    ],
)


@popcorn.pytest.system
def system() -> popcorn.SystemDescription:
    return SYSTEM


# --8<-- [end:describe]


async def test_the_members_run_on_one_clock(system: System) -> None:
    # --8<-- [start:run]
    stop = await system.run(for_=ms(1))
    assert stop.reason is StopReason.TIME
    assert system.members["responder1"].var("node_id").value == 1
    assert system.members["responder2"].var("node_id").value == 2
    # --8<-- [end:run]


async def test_a_tap_shows_every_frame_with_its_times(system: System) -> None:
    # --8<-- [start:tap]
    await system.run(for_=ms(1))
    tap = system.tap("controller.UART4")
    frames = tap.frames(towards="responder1.USART3")
    assert bytes(frame.data for frame in frames[:2]) == b"N\x01"
    assert all(frame.arrival == frame.end + us(20) for frame in frames)
    # --8<-- [end:tap]


async def test_a_cut_loses_the_frames_inside_it(system: System) -> None:
    # --8<-- [start:cut]
    await system.run(for_=ms(1))
    tap = system.tap("controller.UART4")
    tap.frames()
    now = max(system.time_of(end.partition(".")[0]) for end in tap.ends)
    start = VirtualTime(now.ps + ms(15).ps)
    tap.cut(start, VirtualTime(start.ps + ms(30).ps), line="break")
    await system.run(for_=ms(60))
    cut = [frame for frame in tap.frames() if frame.cut]
    assert cut
    assert all(frame.start.ps < start.ps + ms(30).ps for frame in cut)
    # --8<-- [end:cut]


async def test_a_snapshot_restores_every_member(system: System) -> None:
    # --8<-- [start:snapshot]
    await system.run(for_=ms(1))
    snapshot = system.snapshot()
    before = system.members["controller"].time
    await system.run(for_=ms(20))
    system.restore(snapshot)
    assert system.members["controller"].time == before
    # --8<-- [end:snapshot]


async def test_a_member_waits_while_the_system_runs(system: System) -> None:
    # --8<-- [start:running]
    controller = system.members["controller"]
    async with system.running():
        await controller.elapse(ms(1))
    assert system.members["responder1"].var("node_id").value == 1
    # --8<-- [end:running]


@pytest.mark.parametrize(("line", "errors"), [("idle", 0), ("break", 1)])
async def test_a_break_line_ends_the_reception_with_an_error(
    system: System, line: str, errors: int
) -> None:
    await system.run(for_=ms(1))
    tap = system.tap("controller.UART4")
    failed = system.members["responder1"].probe("HAL_UART_ErrorCallback")
    now = max(system.time_of(end.partition(".")[0]) for end in tap.ends)
    start = VirtualTime(now.ps + ms(15).ps)
    tap.cut(start, VirtualTime(start.ps + ms(30).ps), line=cast(LineState, line))
    await system.run(for_=ms(60))
    assert (failed.count > 0) == bool(errors)


async def test_a_script_boots_and_links_in_one_call() -> None:
    system = await SYSTEM.boot()
    try:
        stop = await system.run(for_=ms(1))
        assert stop.reason is StopReason.TIME
        assert system.members["responder1"].var("node_id").value == 1
    finally:
        system.close()
