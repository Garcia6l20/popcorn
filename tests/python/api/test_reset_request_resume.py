"""A run or step after a RESET_REQUEST stop applies that reset first, a call does not."""

from __future__ import annotations

import asyncio

import popcorn
import pytest
from popcorn.units import ms

from .conftest import Machines

RESET = popcorn.StopReason.RESET_REQUEST
BUDGET = popcorn.StopReason.BUDGET
AIRCR = 0xE000_ED0C
SYSRESETREQ = 0x05FA_0004


def requested(machines: Machines) -> tuple[popcorn.Machine, popcorn.Stop]:
    m = machines()
    assert asyncio.run(m.run(budget=10_000)).reason is BUDGET
    m.mem.u32[AIRCR] = SYSRESETREQ
    stop = asyncio.run(m.run(budget=1_000))
    assert stop.reason is RESET
    assert stop.detail == "software"
    assert stop.time.ps > 0
    return m, stop


def fresh(machines: Machines, budget: int) -> popcorn.Stop:
    stop = asyncio.run(machines().run(budget=budget))
    assert stop.reason is BUDGET
    return stop


def same_run(a: popcorn.Stop, b: popcorn.Stop) -> bool:
    return (a.reason, a.time, a.instructions, a.pc) == (b.reason, b.time, b.instructions, b.pc)


def test_a_run_after_a_reset_request_restarts_from_the_reset(machines: Machines) -> None:
    m, first = requested(machines)
    stop = asyncio.run(m.run(budget=3_000))
    assert stop.resets == first.resets + 1
    assert same_run(stop, fresh(machines, 3_000))


def test_a_step_after_a_reset_request_executes_from_the_reset_vector(
    machines: Machines,
) -> None:
    m, first = requested(machines)
    stop = asyncio.run(m.step(1))
    assert stop.resets == first.resets + 1
    assert stop.instructions == 1
    reference = machines()
    assert same_run(stop, asyncio.run(reference.step(1)))


def test_for_counts_from_the_reset_a_run_applies(machines: Machines) -> None:
    m, first = requested(machines)
    stop = asyncio.run(m.run(for_=ms(1)))
    assert stop.reason is popcorn.StopReason.TIME
    assert stop.resets == first.resets + 1
    reference = asyncio.run(machines().run(for_=ms(1)))
    assert same_run(stop, reference)


def test_a_reset_between_clears_the_pending_one(machines: Machines) -> None:
    m, first = requested(machines)
    m.reset(popcorn.ResetKind.PIN)
    stop = asyncio.run(m.run(budget=3_000))
    assert stop.resets == first.resets + 1
    assert same_run(stop, fresh(machines, 3_000))


def test_a_call_after_a_reset_request_keeps_it_pending(machines: Machines) -> None:
    m, first = requested(machines)
    ticks = m.var("ticks").value
    asyncio.run(m.call("tick"))
    assert m.var("ticks").value == ticks + 1
    stop = asyncio.run(m.run(budget=3_000))
    assert stop.resets == first.resets + 1
    assert same_run(stop, fresh(machines, 3_000))


def test_a_snapshot_keeps_the_pending_reset_saved_and_restored(machines: Machines) -> None:
    m, first = requested(machines)
    saved = m.snapshot()
    loaded = popcorn.Snapshot.from_bytes(saved.to_bytes())
    reference = fresh(machines, 3_000)
    for snapshot in (saved, loaded):
        m.reset()
        m.restore(snapshot)
        stop = asyncio.run(m.run(budget=3_000))
        assert stop.resets > first.resets
        assert same_run(stop, reference)
    m.reset()
    asyncio.run(m.run(budget=3_000))
    cleared = m.snapshot()
    m.restore(saved)
    m.restore(cleared)
    stop = asyncio.run(m.run(budget=1_000))
    assert stop.reason is BUDGET
    assert stop.time.ps > reference.time.ps


def test_a_free_run_after_a_reset_request_restarts_from_the_reset(machines: Machines) -> None:
    m, first = requested(machines)

    async def free() -> popcorn.Stop:
        m.start()
        try:
            return await m.wait_for(popcorn.Reached("Reset_Handler"), for_=ms(5))
        finally:
            m.halt()

    stop = asyncio.run(free())
    assert stop.resets == first.resets + 1


def test_until_frame_return_after_a_reset_request_resolves_on_the_reset_core(
    machines: Machines,
) -> None:
    m = machines()
    assert asyncio.run(m.run(until="alu_mark")).reason is popcorn.StopReason.ADDRESS
    m.mem.u32[AIRCR] = SYSRESETREQ
    first = asyncio.run(m.run(budget=1_000))
    assert first.reason is RESET
    assert first.location.symbol == "alu_mark"
    with pytest.raises(popcorn.EmulationError):
        asyncio.run(m.run(until=popcorn.FrameReturn(), budget=1_000))
    stop = asyncio.run(m.run(budget=3_000))
    assert stop.resets == first.resets + 1
    assert same_run(stop, fresh(machines, 3_000))
