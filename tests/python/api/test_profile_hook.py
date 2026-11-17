"""`Machine._profile`, the native profile sink behind `Machine.profile`, and loop reports."""

from __future__ import annotations

import asyncio
import contextlib
import gc
import weakref
from pathlib import Path
from typing import Any

import popcorn
import pytest
from popcorn import System, UartLink, _core
from popcorn._observe import ProfileRecording, open_profile, open_trace
from popcorn.trace import Irq, Loop, LoopEnder, TraceKind, read
from popcorn.units import us

from .conftest import CORE_FIRMWARE, M33_JSON, run_bounded
from .linked import member

SPIN_ELF = CORE_FIRMWARE / "spin.elf"
VECTORS = 0x0800_0000
SYST_CSR = 0xE000_E010
SYST_RVR = 0xE000_E014


def head(m: popcorn.Machine, entry: str, offset: int) -> int:
    return (m.sym(entry) & ~1) + offset


def spinning(entry: str = "wait_ready_ticking") -> popcorn.Machine:
    """The core spin firmware at `entry`. For `wait_ready_ticking`, at its poll of a status
    register, with the SysTick interrupting every 500 core cycles."""
    m = popcorn.Machine(M33_JSON, image=popcorn.Image.elf(SPIN_ELF))
    m.reset()
    asyncio.run(m.run(budget=1_000))
    m.reg.pc = m.sym(entry) & ~1
    if entry == "wait_ready_ticking":
        m.mem.write(VECTORS + 4 * 15, (m.sym("tick_reads") | 1).to_bytes(4, "little"))
        m.mem.write(SYST_RVR, (500).to_bytes(4, "little"))
        m.mem.write(SYST_CSR, (7).to_bytes(4, "little"))
        m.reg.r1 = 0x4002_0000
        m.reg.pc = head(m, entry, 14)
    return m


def profiled(
    m: popcorn.Machine, **options: Any
) -> contextlib.AbstractContextManager[ProfileRecording]:
    return m._profile(**options)  # pyright: ignore[reportPrivateUsage]


def native(m: popcorn.Machine) -> _core.MachineHandle:
    return m._handle  # pyright: ignore[reportPrivateUsage]


def test_a_parked_profile_takes_its_aggregates_at_the_exit() -> None:
    m = spinning()
    start = m.instructions
    with profiled(m, loop_reports=1, burst_window=10_000) as profile:
        assert native(m).loop_reports() == 1
        assert not profile.closed
        with pytest.raises(popcorn.MachineRunning):
            _ = profile.result
        asyncio.run(m.run(budget=6_000))
    assert profile.closed
    assert native(m).loop_reports() == 0
    result = profile.result
    assert result.start == start
    assert result.elapsed == m.instructions - start
    assert sum(block.instructions for block in result.code) == result.instructions
    assert result.exceptions[15].entries == 11
    assert result.exceptions[15].burst > 1
    assert [loop.head for loop in result.loops] == [head(m, "wait_ready_ticking", 16)]
    loop = result.loops[0]
    assert loop.runs == 12
    assert loop.enders == [0, 11, 0, 1, 0, 0]
    assert not result.frames
    profile.close()
    again = profile.result
    assert (again.records, again.instructions, again.elapsed) == (
        result.records,
        result.instructions,
        result.elapsed,
    )


def test_call_stacks_build_a_call_tree() -> None:
    m = spinning("helper_below")
    with profiled(m, call_stacks=True) as profile:
        asyncio.run(m.run(budget=500))
    result = profile.result
    assert result.frames
    assert result.calls > 90
    assert result.loops == []


def test_a_profile_request_below_the_machine_request_holds_until_the_close() -> None:
    m = spinning()
    native(m).set_loop_reports(5)
    with profiled(m, loop_reports=1):
        assert native(m).loop_reports() == 1
        asyncio.run(m.run(budget=1_000))
    assert native(m).loop_reports() == 5
    with profiled(m):
        assert native(m).loop_reports() == 5
    assert native(m).loop_reports() == 5


def test_overlapping_profiles_closed_out_of_order_keep_the_lowest_open_request() -> None:
    m = spinning()
    first = contextlib.ExitStack()
    second = contextlib.ExitStack()
    a = first.enter_context(profiled(m, loop_reports=1))
    b = second.enter_context(profiled(m, loop_reports=3))
    assert native(m).loop_reports() == 1
    asyncio.run(m.run(budget=3_000))
    first.close()
    assert native(m).loop_reports() == 3
    asyncio.run(m.run(budget=3_000))
    second.close()
    assert native(m).loop_reports() == 0
    assert a.result.loops[0].runs == 6
    assert b.result.loops[0].runs == 12
    assert b.result.loops[0].passes > 2 * a.result.loops[0].passes


def loops_of(result: _core.ProfileResult) -> list[tuple[int, int, int, int, list[int]]]:
    return [
        (loop.head, loop.runs, loop.passes, loop.instructions, loop.enders) for loop in result.loops
    ]


def test_a_profile_counts_the_runs_of_its_own_request_whatever_lower_requests_exist() -> None:
    def changing(beside: int) -> tuple[_core.ProfileResult, _core.ProfileResult]:
        m = spinning("poll_value")
        native(m).set_loop_reports(beside)
        with profiled(m, loop_reports=1) as fine, profiled(m, loop_reports=50) as coarse:
            for value in range(11, 23):
                m.periph.TESTDEV.RW.poke(value)
                asyncio.run(m.run(budget=3 * (value % 4) + 4))
            asyncio.run(m.run(budget=300))
            m.periph.TESTDEV.RW.poke(30)
            asyncio.run(m.run(budget=90))
        return fine.result, coarse.result

    fine, alone = changing(0)
    _, beside = changing(1)
    assert fine.loops[0].runs > alone.loops[0].runs
    assert alone.loops[0].passes >= 50 * alone.loops[0].runs
    assert loops_of(beside) == loops_of(alone)


def test_a_dropped_profile_withdraws_its_loop_report_request() -> None:
    m = spinning()
    for asked in (0, 5):
        native(m).set_loop_reports(asked)
        dropped = open_profile(
            native(m),
            call_stacks=False,
            loop_reports=1,
            burst_window=0,
            logged_registers=(),
            write_log_limit=16,
            max_call_depth=8,
        )
        assert native(m).loop_reports() == 1
        asyncio.run(m.run(budget=1_000))
        del dropped
        gc.collect()
        assert native(m).loop_reports() == asked


def test_a_direct_request_while_a_profile_is_open_takes_effect_after_its_close() -> None:
    m = spinning()
    with profiled(m, loop_reports=3):
        native(m).set_loop_reports(7)
        assert native(m).loop_reports() == 3
        native(m).set_loop_reports(1)
        assert native(m).loop_reports() == 1
        native(m).set_loop_reports(0)
        assert native(m).loop_reports() == 3
        native(m).set_loop_reports(7)
    assert native(m).loop_reports() == 7


def test_a_profile_dropped_unclosed_from_a_handler_inside_a_slice_goes_at_the_slice_end() -> None:
    m = spinning()
    held = [
        open_profile(
            native(m),
            call_stacks=True,
            loop_reports=1,
            burst_window=0,
            logged_registers=(),
            write_log_limit=16,
            max_call_depth=8,
        )
    ]
    reports: list[int] = []

    def drop(call: popcorn.Call) -> popcorn.Return:
        if held:
            held.clear()
            gc.collect()
            reports.append(native(m).loop_reports())
        return call.proceed()

    m.intercept("tick_reads", drop)
    asyncio.run(m.run(budget=3_000))
    assert reports == [1]
    assert native(m).loop_reports() == 0


def test_a_trace_dropped_unclosed_from_a_handler_inside_a_slice_writes_until_the_slice_end(
    tmp_path: Path,
) -> None:
    m = spinning()
    path = tmp_path / "dropped.pctr"
    held = [open_trace(native(m), {TraceKind.BLOCK, TraceKind.IRQ}, path, None, None)]
    entries: list[int] = []

    def drop(call: popcorn.Call) -> popcorn.Return:
        if held:
            held.clear()
            gc.collect()
        entries.append(m.instructions)
        return call.proceed()

    m.intercept("tick_reads", drop)
    end = m.instructions + 3_000
    asyncio.run(m.run(budget=3_000))
    records = list(read(path))
    irqs = [record for record in records if isinstance(record, Irq) and record.enter]
    assert len(entries) == 5
    assert [irq.time for irq in irqs] == entries[:1]
    assert max(record.time for record in records) <= end


def test_a_close_from_a_handler_is_refused_and_changes_nothing() -> None:
    m = spinning()
    profile = open_profile(
        native(m),
        call_stacks=False,
        loop_reports=1,
        burst_window=0,
        logged_registers=(),
        write_log_limit=16,
        max_call_depth=8,
    )
    refused: list[bool] = []

    def close(call: popcorn.Call) -> popcorn.Return:
        if not refused:
            with pytest.raises(popcorn.MachineRunning):
                profile.close()
            refused.append(profile.closed)
        return call.proceed()

    m.intercept("tick_reads", close)
    asyncio.run(m.run(budget=3_000))
    assert refused == [False]
    assert native(m).loop_reports() == 1
    profile.close()
    assert profile.result.exceptions[15].entries == 5
    assert native(m).loop_reports() == 0


def test_a_failing_body_still_closes_the_profile() -> None:
    m = spinning()
    with pytest.raises(LookupError), profiled(m, loop_reports=1) as profile:
        asyncio.run(m.run(budget=2_000))
        raise LookupError
    assert profile.closed
    assert profile.result.instructions > 0
    assert native(m).loop_reports() == 0


def test_a_profile_left_inside_a_free_run_closes_at_the_next_boundary() -> None:
    m = spinning()
    end = m.instructions + 20_000
    stack = contextlib.ExitStack()
    profile = stack.enter_context(profiled(m, loop_reports=1))

    async def body() -> None:
        async with m.running():
            await m.wait_until(lambda m: m.instructions >= end)
            stack.close()
            await profile.wait_closed()
            assert profile.closed
            assert profile.result.instructions > 0

    run_bounded(body(), 10)
    assert native(m).loop_reports() == 0


def test_a_profile_left_inside_a_free_run_is_read_after_the_halt() -> None:
    m = spinning()
    end = m.instructions + 20_000
    stack = contextlib.ExitStack()
    profile = stack.enter_context(profiled(m, loop_reports=1))

    async def body() -> None:
        async with m.running():
            await m.wait_until(lambda m: m.instructions >= end)
            stack.close()

    run_bounded(body(), 10)
    assert profile.closed
    result = profile.result
    assert result.elapsed >= 20_000
    assert result.loops[0].enders[1] > 30


def test_a_profile_refuses_bad_options() -> None:
    m = spinning()
    options: list[dict[str, Any]] = [
        {"loop_reports": -1},
        {"loop_reports": 1 << 32},
        {"burst_window": -1},
        {"write_log_limit": -1},
        {"max_call_depth": 1 << 32},
        {"logged_registers": [1 << 16]},
    ]
    for bad in options:
        with pytest.raises(ValueError), profiled(m, **bad):
            pass
    assert native(m).loop_reports() == 0


def test_a_profile_refuses_a_running_machine_and_a_closed_one() -> None:
    m = spinning()

    async def body() -> None:
        async with m.running():
            with pytest.raises(popcorn.MachineRunning), profiled(m):
                pass

    run_bounded(body(), 10)
    m.close()
    with pytest.raises(popcorn.EmulationError), profiled(m):
        pass


def linked(u575: popcorn.PartDescription) -> tuple[System, popcorn.Machine]:
    a = member(u575, "link_ping", "a")
    b = member(u575, "link_echo", "b")
    link = UartLink("a.USART1", "b.USART1", latency=us(20), baud=460_800)
    return System({"a": a, "b": b}, [link]), a


def test_a_profile_refuses_a_member_of_a_linked_system(u575: popcorn.PartDescription) -> None:
    system, a = linked(u575)
    try:
        with pytest.raises(popcorn.ConfigError) as refused, profiled(a):
            pass
        assert refused.value.key == "members.a"
    finally:
        system.close()


def test_a_trace_on_a_member_of_a_linked_system_opens(
    u575: popcorn.PartDescription, tmp_path: Path
) -> None:
    system, a = linked(u575)
    try:
        with a.trace({TraceKind.BLOCK}, tmp_path / "member.pctr") as recording:
            pass
        assert recording.records >= 1
    finally:
        system.close()


def test_a_profile_keeps_no_machine_alive() -> None:
    m = spinning()
    alive = weakref.ref(m)
    with profiled(m, loop_reports=1) as profile:
        asyncio.run(m.run(budget=1_000))
    still_open = open_profile(
        native(m),
        call_stacks=True,
        loop_reports=1,
        burst_window=0,
        logged_registers=(),
        write_log_limit=16,
        max_call_depth=8,
    )
    del m
    gc.collect()
    assert alive() is None
    assert profile.result.instructions > 0
    assert not still_open.closed
    with pytest.raises(popcorn.PopcornError):
        still_open.close()


def loop_rows(path: Path, end: int) -> tuple[list[Loop], Loop]:
    """Loop records before `end`, and the run holding `end`."""
    loops = [record for record in read(path) if isinstance(record, Loop)]
    before = [loop for loop in loops if loop.time < end]
    holding = next(loop for loop in loops if loop.time >= end)
    return before, holding


def test_loop_reports_are_alike_parked_and_running(tmp_path: Path) -> None:
    kinds = {TraceKind.LOOP, TraceKind.IRQ}
    parked = spinning()
    native(parked).set_loop_reports(1)
    with parked.trace(kinds, tmp_path / "parked.pctr"):
        asyncio.run(parked.run(budget=30_000))
    native(parked).set_loop_reports(0)
    end = parked.instructions
    before, holding = loop_rows(tmp_path / "parked.pctr", end)
    assert holding.ender is LoopEnder.END
    assert holding.time == end
    assert len(before) > 50

    running = spinning()
    native(running).set_loop_reports(1)
    stack = contextlib.ExitStack()
    stack.enter_context(running.trace(kinds, tmp_path / "running.pctr"))

    async def body() -> None:
        async with running.running():
            await running.wait_until(lambda m: m.instructions >= end + 2_000)
            stack.close()

    run_bounded(body(), 10)
    native(running).set_loop_reports(0)
    other_before, other_holding = loop_rows(tmp_path / "running.pctr", end)
    assert other_before == before
    assert other_holding.head == holding.head
    assert other_holding.time - other_holding.instructions == holding.time - holding.instructions


def test_a_posted_close_takes_no_gil_on_the_machine_thread() -> None:
    m = spinning()
    stack = contextlib.ExitStack()
    profile = stack.enter_context(profiled(m, loop_reports=1, call_stacks=True))
    before = native(m).gil_entries()

    async def body() -> None:
        async with m.running():
            await asyncio.sleep(0.01)
            stack.close()
            await profile.wait_closed()

    run_bounded(body(), 10)
    assert native(m).gil_entries() == before
    assert profile.result.instructions > 0


def test_the_open_loop_run_reaches_the_profile_as_an_end_record() -> None:
    m = spinning("wait_ready")
    with profiled(m, loop_reports=1) as profile:
        asyncio.run(m.run(budget=3_001))
    loops = profile.result.loops
    assert [loop.head for loop in loops] == [head(m, "wait_ready", 2)]
    assert loops[0].runs == 1
    assert loops[0].enders == [0, 0, 0, 1, 0, 0]
    assert loops[0].passes == 995
