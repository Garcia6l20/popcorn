"""Step out: `run(until=FrameReturn())` stops where the function running now returns."""

from __future__ import annotations

import asyncio

import popcorn
import pytest
from popcorn import FrameReturn, StopReason
from popcorn.units import us

from .conftest import CALLEE_ELF

BUDGET = 1_000_000


def at(m: popcorn.Machine, entry: str, until: str, *, r0: int = 0, lr: int = 0) -> int:
    """Start `entry` with `r0` and `lr`, run to `until`'s entry, return the sp there."""
    m.reg.pc = m.sym(entry) & ~1
    m.reg.r0 = r0
    m.reg.lr = lr
    stop = asyncio.run(m.run(until=until, budget=BUDGET))
    assert stop.reason is StopReason.ADDRESS, stop
    return int(m.reg.sp)


def gil_entries(m: popcorn.Machine) -> int:
    return m._handle.gil_entries()  # pyright: ignore[reportPrivateUsage]


def signed(value: int) -> int:
    return value - (1 << 32) if value >> 31 else value


def test_a_frame_return_stops_at_the_callers_return_address(callee: popcorn.Machine) -> None:
    entry_sp = at(callee, "negated", "negate", r0=5)
    before = gil_entries(callee)
    stop = asyncio.run(callee.run(until=FrameReturn(), budget=BUDGET))
    assert stop.reason is StopReason.RETURNED
    assert stop.detail == "negate returned"
    assert str(stop).endswith(": negate returned")
    assert stop.location.symbol == "negated"
    assert callee.last_stop == stop
    assert int(callee.reg.sp) == entry_sp
    assert signed(int(callee.reg.r0)) == -5
    assert gil_entries(callee) == before


def test_a_frame_return_in_a_recursion_stops_at_the_targeted_frame(
    callee: popcorn.Machine,
) -> None:
    callee.reg.pc = callee.sym("depth") & ~1
    callee.reg.r0 = 4
    for expected in (3, 2, 1):
        stop = asyncio.run(callee.run(until="depth", budget=BUDGET))
        assert stop.reason is StopReason.ADDRESS
        assert int(callee.reg.r0) == expected
    entry_sp = int(callee.reg.sp)
    asyncio.run(callee.step(3))
    stop = asyncio.run(callee.run(until=FrameReturn(), budget=BUDGET))
    assert stop.reason is StopReason.RETURNED
    assert stop.location.symbol == "depth"
    assert int(callee.reg.sp) == entry_sp
    assert int(callee.reg.r0) == 1


def test_a_frame_return_from_an_exception_handler_stops_in_the_interrupted_context(
    callee: popcorn.Machine,
) -> None:
    stop = asyncio.run(callee.run(until="SysTick_Handler", budget=BUDGET))
    assert stop.reason is StopReason.ADDRESS
    assert int(callee.reg.lr) >> 24 == 0xFF
    stop = asyncio.run(callee.run(until=FrameReturn(), budget=BUDGET))
    assert stop.reason is StopReason.RETURNED
    assert stop.detail == "SysTick_Handler returned"
    assert stop.location.symbol == "Reset_Handler"
    assert int(callee.reg.xpsr) & 0x1FF == 0


def test_slicing_does_not_move_a_frame_return(m33: popcorn.PartDescription) -> None:
    def step_out(stop_when: bool) -> tuple[int, int, int]:
        m = popcorn.Machine(m33, image=popcorn.Image.elf(CALLEE_ELF))
        m.reset()
        at(m, "halved", "half")
        stop = asyncio.run(
            m.run(
                until=FrameReturn(),
                budget=BUDGET,
                stop_when=(lambda _: False) if stop_when else None,
            )
        )
        assert stop.reason is StopReason.RETURNED
        return stop.pc, stop.instructions, int(m.reg.r0)

    assert step_out(False) == step_out(True)


def test_a_frame_return_inside_running_is_refused_as_busy(callee: popcorn.Machine) -> None:
    stop = asyncio.run(callee.run(until="SysTick_Handler", budget=BUDGET))
    assert stop.reason is StopReason.ADDRESS

    async def body() -> None:
        async with callee.running():
            with pytest.raises(popcorn.MachineBusy):
                await callee.run(until=FrameReturn())

    asyncio.run(body())


def test_an_unknown_return_point_runs_nothing(callee: popcorn.Machine) -> None:
    callee.reg.pc = callee.sym("forever") & ~1
    callee.reg.lr = 0
    time = callee.time
    with pytest.raises(popcorn.EmulationError, match="forever: cannot find where"):
        asyncio.run(callee.run(until=FrameReturn(), budget=BUDGET))
    assert not callee.busy
    assert callee.time == time
    assert callee.last_stop is None
    stop = asyncio.run(callee.run(budget=1000))
    assert stop.reason is StopReason.BUDGET


def test_a_frame_return_keeps_the_other_bounds_of_the_run(callee: popcorn.Machine) -> None:
    callee.reg.pc = callee.sym("forever") & ~1
    callee.reg.lr = (callee.sym("touched") & ~1) + 6 | 1
    stop = asyncio.run(callee.run(until=FrameReturn(), for_=us(1)))
    assert stop.reason is StopReason.TIME
    callee.reg.pc = callee.sym("forever") & ~1
    stop = asyncio.run(callee.run(until=FrameReturn(), budget=500))
    assert stop.reason is StopReason.BUDGET
