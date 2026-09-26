"""The concurrency rules: one run at a time, no sync access while running, cancellation."""

from __future__ import annotations

import asyncio
import time

import popcorn
import pytest
from popcorn.units import ms

from .conftest import Machines


def test_a_second_concurrent_run_is_machine_busy(machines: Machines) -> None:
    m = machines()

    async def scenario() -> None:
        first = asyncio.create_task(m.run())
        await asyncio.sleep(0)
        with pytest.raises(popcorn.MachineBusy) as caught:
            await m.run(budget=10)
        assert isinstance(caught.value, popcorn.PopcornError)
        with pytest.raises(popcorn.MachineBusy):
            await m.step()
        first.cancel()
        with pytest.raises(asyncio.CancelledError):
            await first

    asyncio.run(scenario())


def test_sync_access_from_another_task_during_a_run_is_machine_running(
    machines: Machines,
) -> None:
    m = machines()

    async def scenario() -> None:
        first = asyncio.create_task(m.run())
        await asyncio.sleep(0)
        with pytest.raises(popcorn.MachineRunning) as caught:
            m.reset()
        assert isinstance(caught.value, popcorn.PopcornError)
        with pytest.raises(popcorn.MachineRunning):
            _ = m.instructions
        first.cancel()
        with pytest.raises(asyncio.CancelledError):
            await first

    asyncio.run(scenario())


def test_a_handler_reads_the_machine_but_the_core_refuses_a_reset(machines: Machines) -> None:
    m = machines(slice_instructions=1_000)
    seen: list[int] = []

    def predicate(machine: popcorn.Machine) -> bool:
        seen.append(machine.instructions)
        machine.reset()
        return False

    with pytest.raises(popcorn.MachineRunning, match="running a slice"):
        asyncio.run(m.run(stop_when=predicate, budget=100_000))
    assert seen
    assert not m.busy


def test_cancelling_a_run_leaves_the_machine_resumable(machines: Machines) -> None:
    m = machines()

    async def scenario() -> int:
        task = asyncio.create_task(m.run())
        await asyncio.sleep(0.005)
        task.cancel()
        with pytest.raises(asyncio.CancelledError):
            await task
        return m.instructions

    cancelled_at = asyncio.run(scenario())
    assert cancelled_at > 0
    stop = asyncio.run(m.run(budget=1000))
    assert stop.reason is popcorn.StopReason.BUDGET
    assert stop.instructions >= cancelled_at + 1000


def test_the_event_loop_keeps_ticking_during_a_run(machines: Machines) -> None:
    m = machines()
    gaps: list[float] = []

    async def ticker() -> None:
        last = time.perf_counter()
        while True:
            await asyncio.sleep(0)
            now = time.perf_counter()
            gaps.append(now - last)
            last = now

    async def scenario() -> None:
        tick = asyncio.create_task(ticker())
        run = asyncio.create_task(m.run())
        await asyncio.sleep(0.05)
        run.cancel()
        tick.cancel()
        await asyncio.gather(run, tick, return_exceptions=True)

    asyncio.run(scenario())
    assert len(gaps) > 5
    assert max(gaps) < 0.020


def test_running_advances_the_machine_in_the_background(machines: Machines) -> None:
    m = machines()
    before = m.instructions

    async def scenario() -> None:
        async with m.running():
            await asyncio.sleep(0.01)
            with pytest.raises(popcorn.MachineRunning):
                m.reset()

    asyncio.run(scenario())
    assert m.instructions > before
    assert not m.busy


def test_running_waits_through_idle(machines: Machines) -> None:
    m = machines("idle")

    async def scenario() -> None:
        async with m.running():
            await asyncio.sleep(0.005)

    asyncio.run(scenario())


def test_a_fault_inside_running_raises_on_exit(machines: Machines) -> None:
    m = machines("fault")

    async def scenario() -> None:
        async with m.running():
            await asyncio.sleep(0.005)

    with pytest.raises(popcorn.EmulationError) as caught:
        asyncio.run(scenario())
    assert caught.value.stop is not None
    assert caught.value.stop.reason is popcorn.StopReason.FAULT
    assert caught.value.reason is popcorn.StopReason.FAULT
    assert caught.value.symbol == "fault_main"
    assert isinstance(caught.value, popcorn.FaultError)
    assert caught.value.kind is popcorn.FaultKind.BUS
    assert caught.value.address == 0x6000_0000


@pytest.mark.xfail(
    strict=True,
    raises=TimeoutError,
    reason="run_state::idle_jump stops IDLE without jumping to until_time when no event is due",
)
def test_a_deadline_inside_running_advances_an_idle_machine(machines: Machines) -> None:
    m = machines("idle")
    start = m.time

    async def scenario() -> popcorn.Stop:
        async with m.running():
            return await m.wait_for(popcorn.Reached("alu_mark"), for_=ms(1))

    stop = asyncio.run(asyncio.wait_for(scenario(), timeout=2))
    assert stop.reason is popcorn.StopReason.TIME
    assert stop.time >= start + ms(1)


def test_a_body_error_inside_running_carries_the_background_stop(machines: Machines) -> None:
    m = machines("fault")

    async def scenario() -> None:
        async with m.running():
            await asyncio.sleep(0.005)
            raise KeyError("body failed")

    with pytest.raises(KeyError) as caught:
        asyncio.run(scenario())
    assert any("background run stopped: FAULT" in note for note in caught.value.__notes__)
    assert not m.busy


def test_a_cancelled_running_carries_the_background_stop(machines: Machines) -> None:
    m = machines("fault")

    async def body() -> None:
        async with m.running():
            await asyncio.sleep(10)

    async def scenario() -> asyncio.CancelledError:
        task = asyncio.create_task(body())
        await asyncio.sleep(0.005)
        task.cancel()
        try:
            await task
        except asyncio.CancelledError as cancelled:
            return cancelled
        raise AssertionError("the body was not cancelled")

    cancelled = asyncio.run(scenario())
    assert any("background run stopped: FAULT" in note for note in cancelled.__notes__)
    assert not m.busy


class AfterInstructions:
    """Ready once the machine has executed a count of instructions."""

    def __init__(self, count: int) -> None:
        self.count = count

    def ready(self, machine: popcorn.Machine) -> bool:
        """Whether the count is reached."""
        return machine.instructions >= self.count


def test_wait_for_inside_running_awaits_the_driver(machines: Machines) -> None:
    m = machines(slice_instructions=10_000)

    async def scenario() -> tuple[popcorn.Stop, popcorn.Stop]:
        async with m.running():
            ready = await m.wait_for(AfterInstructions(50_000))
            timed = await m.wait_for(AfterInstructions(10**12), for_=ms(1))
        return ready, timed

    ready, timed = asyncio.run(scenario())
    assert ready.reason is popcorn.StopReason.USER
    assert 50_000 <= ready.instructions < 50_000 + 10_000 + 64
    assert timed.reason is popcorn.StopReason.TIME


def test_a_run_inside_running_is_machine_busy(machines: Machines) -> None:
    m = machines()

    async def scenario() -> None:
        async with m.running():
            with pytest.raises(popcorn.MachineBusy):
                await m.run(budget=10)

    asyncio.run(scenario())
