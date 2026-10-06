"""Accesses of a hosted machine: sync parked, refused running, `m.read` and `m.write` over
`m.ref`, and the run control around them."""

from __future__ import annotations

import asyncio
import gc
import weakref

import popcorn
import pytest
from popcorn.units import VirtualTime, ms, us

from .conftest import Machines, run_bounded

SRAM = 0x2000_8000
UNMAPPED = 0x6000_0000


class Never:
    def ready(self, machine: popcorn.Machine) -> bool:
        return False


def test_sync_accesses_inside_running_are_machine_running(machines: Machines) -> None:
    m = machines(slice_instructions=1_000)

    async def scenario() -> None:
        async with m.running():
            with pytest.raises(popcorn.MachineRunning, match=r"await m\.read"):
                _ = m.mem.u32[SRAM]
            with pytest.raises(popcorn.MachineRunning, match=r"await m\.read"):
                m.reg.r0 = 1

    asyncio.run(scenario())


def test_sync_accesses_stay_refused_after_the_free_run_ended_by_itself(
    machines: Machines,
) -> None:
    m = machines("fault")

    async def scenario() -> None:
        async with m.running():
            ended = await m.wait_for(Never())
            assert ended.reason is popcorn.StopReason.FAULT
            assert m.busy
            with pytest.raises(popcorn.MachineRunning):
                _ = m.pc
            with pytest.raises(popcorn.MachineRunning):
                m.mem.u32[SRAM] = 1
            (pc,) = await m.read(m.ref.pc)
            assert pc == ended.pc

    with pytest.raises(popcorn.FaultError):
        run_bounded(scenario())
    assert not m.busy


def test_a_parked_read_returns_at_once_at_the_machine_time(machines: Machines) -> None:
    m = machines()
    pending = m.read(m.ref.pc, m.ref.instructions)
    assert pending.done()
    values = asyncio.run(pending)
    assert values == (m.pc, m.instructions)
    assert values.time == m.time
    assert isinstance(values, popcorn.Readout)
    assert asyncio.run(m.read()).time == m.time


def test_two_writes_of_one_register_apply_in_order(machines: Machines) -> None:
    m = machines()
    asyncio.run(m.write((m.ref.reg.r0, 1), (m.ref.reg.r0, 2)))
    assert m.reg.r0 == 2


def test_a_failing_op_stops_the_batch_after_the_ops_before_it(machines: Machines) -> None:
    m = machines()
    m.mem.u32[SRAM] = 0
    with pytest.raises(popcorn.EmulationError) as caught:
        asyncio.run(m.write((m.ref.mem.u32[SRAM], 7), (m.ref.mem.u32[UNMAPPED], 1)))
    assert any("argument 1" in note for note in caught.value.__notes__)
    assert m.mem.u32[SRAM] == 7


def test_a_failing_parked_batch_leaves_its_machine_collectable(machines: Machines) -> None:
    m = machines()
    with pytest.raises(popcorn.EmulationError):
        asyncio.run(m.write((m.ref.mem.u32[SRAM], 7), (m.ref.mem.u32[UNMAPPED], 1)))
    with pytest.raises(popcorn.EmulationError):
        asyncio.run(m.read(m.ref.mem.u32[UNMAPPED]))
    probe = weakref.ref(m)
    del m
    gc.collect()
    assert probe() is None


def test_done_follows_the_command_not_the_await(machines: Machines) -> None:
    m = machines("idle")
    asyncio.run(m.run())
    pending = m.read(m.ref.pc, at=m.time + us(100))

    async def scenario() -> bool:
        task = asyncio.ensure_future(pending)
        await asyncio.sleep(0)
        started = pending.done()
        await m.run(for_=ms(1))
        await task
        return started

    assert not asyncio.run(scenario())
    assert pending.done()


def test_a_past_stamp_on_a_parked_machine_raises_at_the_call(machines: Machines) -> None:
    m = machines()
    asyncio.run(m.run(for_=us(10)))
    with pytest.raises(ValueError, match="before machine time"):
        m.write((m.ref.reg.r0, 1), at=VirtualTime(0))


def test_a_stamped_write_waits_for_the_run_that_reaches_it(machines: Machines) -> None:
    m = machines()
    at = m.time + us(5)
    pending = m.write((m.ref.mem.u32[SRAM], 0x5A), at=at)
    assert not pending.done()

    async def scenario() -> VirtualTime:
        await m.run(for_=us(20))
        return (await pending).time

    assert asyncio.run(scenario()) >= at
    assert m.mem.u32[SRAM] == 0x5A


def test_a_stamped_read_on_an_idle_core_runs_exactly_at_its_time(machines: Machines) -> None:
    m = machines("idle")
    asyncio.run(m.run())
    at = m.time + us(250)
    pending = m.read(m.ref.instructions, at=at)

    async def scenario() -> VirtualTime:
        await m.run(for_=ms(1))
        return (await pending).time

    assert asyncio.run(scenario()) == at


@pytest.mark.parametrize("move", ["reset", "restore"])
def test_a_stamped_write_superseded_while_parked_raises(machines: Machines, move: str) -> None:
    m = machines()
    saved = m.snapshot()
    pending = m.write((m.ref.reg.r0, 1), at=m.time + us(5))
    if move == "reset":
        m.reset()
    else:
        m.restore(saved)

    async def scenario() -> None:
        await m.run(for_=us(20))
        await pending

    with pytest.raises(popcorn.EmulationError, match=f"superseded by a {move}"):
        asyncio.run(scenario())


def test_a_cancelled_stamped_write_never_applies(machines: Machines) -> None:
    m = machines()
    m.mem.u32[SRAM] = 0

    async def scenario() -> None:
        task = asyncio.ensure_future(m.write((m.ref.mem.u32[SRAM], 9), at=m.time + us(5)))
        await asyncio.sleep(0)
        task.cancel()
        with pytest.raises(asyncio.CancelledError):
            await task
        await m.run(for_=us(20))

    asyncio.run(scenario())
    assert m.mem.u32[SRAM] == 0


def test_read_and_write_from_a_handler_raise(machines: Machines) -> None:
    m = machines(slice_instructions=1_000)
    other = machines()
    errors: list[BaseException] = []

    def predicate(machine: popcorn.Machine) -> bool:
        for target in (machine, other):
            try:
                target.read(target.ref.pc)
            except RuntimeError as error:
                errors.append(error)
        return True

    asyncio.run(m.run(stop_when=predicate))
    assert len(errors) == 2
    assert all("handler accesses the machine directly" in str(e) for e in errors)


def test_a_ref_of_another_machine_is_refused(machines: Machines) -> None:
    m, other = machines(), machines()
    with pytest.raises(ValueError, match="another machine"):
        m.read(other.ref.pc)
    with pytest.raises(TypeError, match="read-only"):
        m.write((m.ref.pc, 0))
    with pytest.raises(TypeError, match="pair"):
        m.write()


def test_halt_start_and_running_edges(machines: Machines) -> None:
    m = machines()
    m.halt()
    m.start()
    try:
        with pytest.raises(popcorn.MachineBusy):
            m.start()
    finally:
        m.halt()
    assert not m.busy

    faulty = machines("fault")

    async def faulting() -> None:
        async with faulty.running():
            await asyncio.sleep(0.01)

    with pytest.raises(popcorn.FaultError):
        run_bounded(faulting())

    ended = machines("fault")

    async def waiting() -> tuple[popcorn.Stop, popcorn.Stop]:
        ended.start()
        try:
            first = await ended.wait_for(Never())
            return first, await ended.wait_for(Never(), for_=ms(1))
        finally:
            with pytest.raises(popcorn.FaultError):
                ended.halt()

    first, second = run_bounded(waiting())
    assert first == second
    assert first.reason is popcorn.StopReason.FAULT


def test_a_cancelled_call_leaves_no_call_in_progress(callee: popcorn.Machine) -> None:
    async def scenario() -> None:
        task = asyncio.create_task(callee.call("forever"))
        await asyncio.sleep(0.005)
        task.cancel()
        with pytest.raises(asyncio.CancelledError):
            await task

    asyncio.run(scenario())
    assert not callee._handle.in_call()  # pyright: ignore[reportPrivateUsage]
