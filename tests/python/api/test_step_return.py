"""Step out: `run(until=FrameReturn())` stops where the function running now returns,
`step_return()` also gives the value."""

from __future__ import annotations

import asyncio
import struct
from pathlib import Path

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


def test_a_shrink_wrapped_early_exit_on_its_return_steps_out(callee: popcorn.Machine) -> None:
    callee.reg.pc = callee.sym("depth") & ~1
    callee.reg.r0 = 1
    assert asyncio.run(callee.run(until="depth", budget=BUDGET)).reason is StopReason.ADDRESS
    assert int(callee.reg.r0) == 0
    asyncio.run(callee.step(1))
    assert callee.where().offset > 4
    assert asyncio.run(callee.step_return()) == 0
    stop = callee.last_stop
    assert stop is not None
    assert stop.location.symbol == "depth"


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


def word(value: float, fmt: str) -> int:
    packed = struct.pack(f"<{fmt}", value)
    return int.from_bytes(packed, "little")


def into_negated(m: popcorn.Machine, fn: str) -> None:
    """Stop at `fn`'s entry as if `negated` had called it."""
    at(m, "negated", "negate", r0=5)
    m.reg.pc = m.sym(fn) & ~1


def test_step_return_gives_the_dwarf_typed_value(callee: popcorn.Machine) -> None:
    at(callee, "negated", "negate", r0=5)
    before = gil_entries(callee)
    assert asyncio.run(callee.step_return()) == -5
    assert gil_entries(callee) == before
    stop = callee.last_stop
    assert stop is not None
    assert stop.reason is StopReason.RETURNED
    assert stop.location.symbol == "negated"
    into_negated(callee, "ones")
    assert asyncio.run(callee.step_return()) == 0xFFFF_FFFF


def test_step_return_gives_floats_and_doubles(callee: popcorn.Machine) -> None:
    callee.reg.pc = callee.sym("scaled") & ~1
    callee.reg.s0 = word(1.5, "f")
    assert asyncio.run(callee.run(until="mulf")).reason is StopReason.ADDRESS
    assert asyncio.run(callee.step_return()) == 3.0
    callee.reg.pc = callee.sym("halved") & ~1
    wide = word(3.0, "d")
    callee.reg.s0 = wide & 0xFFFF_FFFF
    callee.reg.s1 = wide >> 32
    assert asyncio.run(callee.run(until="half")).reason is StopReason.ADDRESS
    assert asyncio.run(callee.step_return()) == 1.5


def test_step_return_gives_none_for_a_void_function_and_a_handler(
    callee: popcorn.Machine,
) -> None:
    at(callee, "touched", "touch")
    assert asyncio.run(callee.step_return()) is None
    assert callee.mem.u32[callee.sym("flag")] == 2
    callee.reset()
    assert asyncio.run(callee.run(until="SysTick_Handler")).reason is StopReason.ADDRESS
    assert asyncio.run(callee.step_return()) is None
    assert callee.where().symbol == "Reset_Handler"


def test_step_return_without_dwarf_gives_r0_as_i32(
    m33: popcorn.PartDescription, tmp_path: Path
) -> None:
    data = CALLEE_ELF.read_bytes()
    assert data.count(b".debug_info\0") == 1
    stripped = tmp_path / "callee.elf"
    stripped.write_bytes(data.replace(b".debug_info\0", b".nodbg_info\0"))
    m = popcorn.Machine(m33, image=popcorn.Image.elf(stripped))
    m.reset()
    into_negated(m, "ones")
    assert asyncio.run(m.step_return()) == -1
    m.reset()
    assert asyncio.run(m.run(until="SysTick_Handler")).reason is StopReason.ADDRESS
    assert asyncio.run(m.step_return()) is None


def test_small_slices_give_the_same_step_out(m33: popcorn.PartDescription) -> None:
    def step_out(slice_instructions: int) -> tuple[int | float | None, int, int]:
        m = popcorn.Machine(
            m33, image=popcorn.Image.elf(CALLEE_ELF), slice_instructions=slice_instructions
        )
        m.reset()
        at(m, "halved", "__aeabi_dadd")
        value = asyncio.run(m.step_return())
        stop = m.last_stop
        assert stop is not None
        return value, stop.pc, stop.instructions

    assert step_out(7) == step_out(1_000_000)


def test_step_return_raises_for_another_stop(callee: popcorn.Machine) -> None:
    callee.reg.pc = callee.sym("forever") & ~1
    callee.reg.lr = 0
    with pytest.raises(popcorn.EmulationError, match="forever: cannot find where"):
        asyncio.run(callee.step_return())
    into_negated(callee, "forever")
    with pytest.raises(popcorn.BudgetExceeded, match="the step out of forever"):
        asyncio.run(callee.step_return(budget=1000))
    into_negated(callee, "fault")
    with pytest.raises(popcorn.FaultError, match="the step out of fault"):
        asyncio.run(callee.step_return())


def test_step_return_inside_running_is_refused_as_busy(callee: popcorn.Machine) -> None:
    assert asyncio.run(callee.run(until="SysTick_Handler")).reason is StopReason.ADDRESS

    async def body() -> None:
        async with callee.running():
            with pytest.raises(popcorn.MachineBusy):
                await callee.step_return()

    asyncio.run(body())


def test_step_return_refuses_a_struct_return_before_running(callee: popcorn.Machine) -> None:
    at(callee, "use_pair", "make_pair", r0=3)
    pc, time = callee.pc, callee.time
    with pytest.raises(NotImplementedError, match="struct"):
        asyncio.run(callee.step_return())
    assert (callee.pc, callee.time) == (pc, time)
    assert not callee.busy
    stop = asyncio.run(callee.run(until=FrameReturn(), budget=BUDGET))
    assert stop.reason is StopReason.RETURNED
