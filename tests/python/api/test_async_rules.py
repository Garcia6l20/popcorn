"""The concurrency rules: one run at a time, no sync access while running, cancellation."""

from __future__ import annotations

import asyncio
import itertools
import threading
import time

import popcorn
import pytest
from popcorn.units import VirtualTime, ms

from .conftest import Machines, run_bounded


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
            _ = m.last_stop
        first.cancel()
        with pytest.raises(asyncio.CancelledError):
            await first

    asyncio.run(scenario())


def test_a_handler_reads_the_machine_but_the_core_refuses_a_reset(machines: Machines) -> None:
    m = machines(slice_instructions=1_000)
    seen: list[int] = []

    def predicate(machine: popcorn.Machine) -> bool:
        seen.append(machine.pc)
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


def test_time_reads_inside_running_at_the_point_the_run_reached(machines: Machines) -> None:
    m = machines(slice_instructions=10_000)

    async def scenario() -> tuple[list[VirtualTime], popcorn.Stop, VirtualTime]:
        times = [m.time]
        async with m.running():
            for _ in range(50):
                await asyncio.sleep(0)
                times.append(m.time)
            stop = await m.wait_for(AfterInstructions(10**12), for_=ms(1))
            at_stop = m.time
            with pytest.raises(popcorn.MachineRunning, match="the run's thread"):
                await asyncio.to_thread(lambda: m.time)
        return times, stop, at_stop

    times, stop, at_stop = asyncio.run(scenario())
    assert all(a <= b for a, b in itertools.pairwise(times))
    assert times[-1] > times[0]
    assert stop.reason is popcorn.StopReason.TIME
    assert at_stop == stop.time
    assert m.time >= at_stop


def test_time_reads_from_another_task_during_a_run(machines: Machines) -> None:
    m = machines(slice_instructions=1_000)

    async def scenario() -> list[VirtualTime]:
        run = asyncio.create_task(m.run(for_=ms(5)))
        times: list[VirtualTime] = []
        while not run.done():
            times.append(m.time)
            await asyncio.sleep(0)
        stop = await run
        times.append(stop.time)
        return times

    times = asyncio.run(scenario())
    assert len(times) > 2
    assert all(a <= b for a, b in itertools.pairwise(times))
    assert m.time == times[-1]


def test_time_from_the_building_thread_during_a_run_on_another_thread_is_machine_running(
    machines: Machines,
) -> None:
    m = machines(slice_instructions=1_000)
    done = threading.Event()

    async def scenario() -> popcorn.Stop:
        run = asyncio.create_task(
            asyncio.to_thread(asyncio.run, m.run(stop_when=lambda _: done.is_set()))
        )
        while not m.busy:
            await asyncio.sleep(0.001)
        try:
            with pytest.raises(popcorn.MachineRunning, match="the run's thread"):
                _ = m.time
        finally:
            done.set()
        return await run

    stop = asyncio.run(scenario())
    assert stop.reason is popcorn.StopReason.USER
    assert m.time == stop.time


def test_a_handler_of_a_run_on_another_thread_reads_the_time(machines: Machines) -> None:
    m = machines(slice_instructions=1_000)
    seen: list[VirtualTime] = []

    def predicate(machine: popcorn.Machine) -> bool:
        seen.append(machine.time)
        return len(seen) >= 5

    stop = asyncio.run(asyncio.to_thread(asyncio.run, m.run(stop_when=predicate)))
    assert stop.reason is popcorn.StopReason.USER
    assert len(seen) == 5
    assert all(a <= b for a, b in itertools.pairwise(seen))
    assert seen[-1] <= stop.time


def test_memory_reads_inside_running_at_the_point_the_run_reached(machines: Machines) -> None:
    m = machines(slice_instructions=10_000)
    value = m.var("alu_value")
    address = m.sym("alu_value")

    async def scenario() -> tuple[list[int], list[int], int, int, int]:
        values: list[int] = []
        counts: list[int] = []
        async with m.running():
            for _ in range(50):
                await asyncio.sleep(0)
                values.append(int(value.value))
                counts.append(m.instructions)
            data = m.mem.read(address, 4)
            raw = int.from_bytes(data, "little")
            word = m.mem.u32[address]
            pc = m.pc
            assert m.reg.pc == pc
            assert m.pending_events >= 0
            text = data.split(b"\0")[0].decode("utf-8", errors="replace")
            assert m.mem.cstring(address, 4) == text
        return values, counts, raw, word, m.instructions

    values, counts, raw, word, after = asyncio.run(scenario())
    assert len(set(values)) > 1
    assert all(a <= b for a, b in itertools.pairwise(counts))
    assert raw == word == values[-1] == value.value
    assert counts[-1] == after


def test_a_handler_reads_memory_and_variables(machines: Machines) -> None:
    m = machines(slice_instructions=1_000)
    seen: list[int] = []

    def predicate(machine: popcorn.Machine) -> bool:
        seen.append(int(machine.var("alu_value").value))
        assert machine.mem.u32[machine.sym("alu_value")] == seen[-1]
        return len(seen) >= 5

    stop = asyncio.run(m.run(stop_when=predicate))
    assert stop.reason is popcorn.StopReason.USER
    assert len(set(seen)) > 1
    assert m.var("alu_value").value == seen[-1]


def test_reads_from_another_thread_during_a_run_are_machine_running(machines: Machines) -> None:
    m = machines(slice_instructions=1_000)
    address = m.sym("alu_value")
    reads = (
        lambda: m.mem.u32[address],
        lambda: m.mem.read(address, 4),
        lambda: m.var("alu_value").value,
        lambda: m.instructions,
        lambda: m.pc,
        lambda: m.reg.r0,
        lambda: m.pending_events,
        lambda: m.periph.TESTDEV.RW.value,
        lambda: m.periph.TESTDEV.pin(0).level,
        lambda: m.periph.TESTDEV.pin(0).mode,
        lambda: m.periph.TESTDEV.pin(0).alternate,
    )

    async def scenario() -> None:
        async with m.running():
            for read in reads:
                with pytest.raises(popcorn.MachineRunning, match="the run's thread"):
                    await asyncio.to_thread(read)

    asyncio.run(scenario())


def test_writes_and_bus_reads_inside_running_are_machine_running(machines: Machines) -> None:
    m = machines(slice_instructions=1_000)
    value = m.var("alu_value")
    address = m.sym("alu_value")
    testdev = m.description.peripherals[0].base
    sram = next(r for r in m.description.regions if r.name == "SRAM")

    async def scenario() -> None:
        async with m.running():
            await asyncio.sleep(0)
            with pytest.raises(popcorn.MachineRunning, match="stop or await"):
                m.mem.u32[address] = 1
            with pytest.raises(popcorn.MachineRunning, match="stop or await"):
                m.mem.write(address, b"\0")
            with pytest.raises(popcorn.MachineRunning, match="stop or await"):
                value.value = 1
            with pytest.raises(popcorn.MachineRunning, match="stop or await"):
                m.reg.r0 = 1
            with pytest.raises(popcorn.MachineRunning, match="stop or await"):
                m.periph.TESTDEV.RW.value = 1
            with pytest.raises(popcorn.MachineRunning, match="memory regions"):
                _ = m.mem.u32[testdev]
            with pytest.raises(popcorn.MachineRunning, match="memory regions"):
                _ = m.periph.TESTDEV.RW.read()
            with pytest.raises(popcorn.MachineRunning, match="memory regions"):
                _ = m.nvic.pending(0)
            with pytest.raises(popcorn.MachineRunning, match="stop or await"):
                m.periph.TESTDEV.RW.write(1)
            with pytest.raises(popcorn.MachineRunning, match="memory regions"):
                _ = m.mem.read(sram.start + sram.size - 4, 16)

    asyncio.run(scenario())


def test_a_run_inside_running_is_machine_busy(machines: Machines) -> None:
    m = machines()

    async def scenario() -> None:
        async with m.running():
            with pytest.raises(popcorn.MachineBusy):
                await m.run(budget=10)

    asyncio.run(scenario())


def boom(_: object) -> None:
    raise ValueError("handler failed")


class Recorded:
    """A handler raising ValueError, counting its calls."""

    def __init__(self) -> None:
        self.calls = 0

    def __call__(self, _: object) -> None:
        """Count the call and fail."""
        self.calls += 1
        raise ValueError("handler failed")


class Never:
    """Never ready."""

    def ready(self, machine: popcorn.Machine) -> bool:
        """Always False."""
        return False


def test_an_on_return_error_inside_running_raises_at_the_next_wait(machines: Machines) -> None:
    m = machines(slice_instructions=1_000)
    m.on_return("alu_mark", boom)

    async def scenario() -> None:
        async with m.running():
            await m.wait_for(Never(), for_=ms(100))

    with pytest.raises(ValueError, match="handler failed"):
        run_bounded(scenario())
    assert not m.busy


def test_an_intercept_error_inside_running_raises_at_the_next_wait(machines: Machines) -> None:
    m = machines(slice_instructions=1_000)

    def failing(_: popcorn.Call) -> popcorn.Return:
        raise ValueError("handler failed")

    m.intercept("alu_mark", failing)

    async def scenario() -> None:
        async with m.running():
            await m.wait_for(Never(), for_=ms(100))

    with pytest.raises(ValueError, match="handler failed"):
        run_bounded(scenario())
    assert not m.busy


def test_a_watch_error_inside_running_raises_at_the_next_wait(machines: Machines) -> None:
    m = machines(slice_instructions=1_000)
    m.watch("alu_rounds", handler=boom)

    async def scenario() -> None:
        async with m.running():
            await m.wait_for(popcorn.Reached("idle_main"), for_=ms(100))

    with pytest.raises(ValueError, match="handler failed"):
        run_bounded(scenario())
    assert not m.busy


def test_a_handler_error_inside_running_raises_on_exit_without_a_wait(
    machines: Machines,
) -> None:
    m = machines(slice_instructions=1_000)
    handler = Recorded()
    m.on_return("alu_mark", handler)
    instructions: list[int] = []

    async def scenario() -> None:
        async with m.running():
            while not handler.calls:
                await asyncio.sleep(0.001)
            instructions.append(m.instructions)
            await asyncio.sleep(0.01)
            instructions.append(m.instructions)

    with pytest.raises(ValueError, match="handler failed"):
        run_bounded(scenario())
    assert instructions[0] == instructions[1]
    assert not m.busy


def test_every_wait_after_a_handler_error_raises_it(machines: Machines) -> None:
    m = machines(slice_instructions=1_000)
    m.on_return("alu_mark", boom)
    errors: list[BaseException] = []

    async def scenario() -> None:
        async with m.running():
            first, second = await asyncio.gather(
                m.wait_for(Never(), for_=ms(100)),
                m.wait_for(Never()),
                return_exceptions=True,
            )
            assert isinstance(first, ValueError)
            assert isinstance(second, ValueError)
            errors.extend((first, second))
            await m.wait_for(Never())

    with pytest.raises(ValueError, match="handler failed") as caught:
        run_bounded(scenario())
    assert errors == [caught.value, caught.value]
    assert not m.busy


def test_a_body_error_inside_running_carries_the_handler_error(machines: Machines) -> None:
    m = machines(slice_instructions=1_000)
    handler = Recorded()
    m.on_return("alu_mark", handler)

    async def scenario() -> None:
        async with m.running():
            while not handler.calls:
                await asyncio.sleep(0.001)
            raise KeyError("body failed")

    with pytest.raises(KeyError) as caught:
        run_bounded(scenario())
    assert caught.value.__notes__ == ["background run raised: ValueError('handler failed')"]


def test_an_event_error_inside_running_fails_only_its_wait(machines: Machines) -> None:
    m = machines(slice_instructions=1_000)

    class Broken:
        def ready(self, machine: popcorn.Machine) -> bool:
            raise ValueError("event failed")

    async def scenario() -> popcorn.Stop:
        async with m.running():
            with pytest.raises(ValueError, match="event failed"):
                await m.wait_for(Broken())
            return await m.wait_for(AfterInstructions(50_000))

    assert run_bounded(scenario()).reason is popcorn.StopReason.USER


def test_a_wait_after_the_background_run_stopped_returns_that_stop(machines: Machines) -> None:
    m = machines("fault")

    async def scenario() -> popcorn.Stop | None:
        async with m.running():
            while m.instructions == 0:
                await asyncio.sleep(0.001)
            await asyncio.sleep(0.01)
            return await m.wait_for(Never())
        return None

    with pytest.raises(popcorn.FaultError):
        run_bounded(scenario())
    assert not m.busy


def test_a_pytest_failure_in_a_handler_inside_running_fails_the_wait(
    machines: Machines,
) -> None:
    m = machines(slice_instructions=1_000)

    def failing(_: object) -> None:
        pytest.fail("checked in the handler")

    m.on_return("alu_mark", failing)

    async def scenario() -> None:
        async with m.running():
            await m.wait_for(Never(), for_=ms(100))

    with pytest.raises(pytest.fail.Exception, match="checked in the handler"):
        run_bounded(scenario())
    assert not m.busy


def test_a_keyboard_interrupt_in_a_handler_inside_running_reaches_the_caller(
    machines: Machines,
) -> None:
    m = machines(slice_instructions=1_000)

    def interrupted(_: object) -> None:
        raise KeyboardInterrupt

    m.on_return("alu_mark", interrupted)

    async def scenario() -> None:
        async with m.running():
            await m.wait_for(Never(), for_=ms(100))

    with pytest.raises(KeyboardInterrupt):
        run_bounded(scenario())
    assert not m.busy


def test_a_machine_runs_again_after_a_handler_error_inside_running(machines: Machines) -> None:
    m = machines(slice_instructions=1_000)
    saved = m.snapshot()
    hook = m.on_return("alu_mark", boom)

    async def failing() -> None:
        async with m.running():
            await m.wait_for(Never(), for_=ms(100))

    async def passing() -> popcorn.Stop:
        async with m.running():
            return await m.wait_for(AfterInstructions(m.instructions + 50_000), for_=ms(100))

    with pytest.raises(ValueError, match="handler failed"):
        run_bounded(failing())
    hook.remove()
    assert run_bounded(passing()).reason is popcorn.StopReason.USER

    m.on_return("alu_mark", boom)
    m.restore(saved)
    with pytest.raises(ValueError, match="handler failed"):
        run_bounded(failing())
    m.remove_hooks()
    m.restore(saved)
    assert run_bounded(passing()).reason is popcorn.StopReason.USER
    assert not m.busy
