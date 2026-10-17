"""The rules of a hosted machine.

Every machine runs on its own thread. One run, step, call, wait or free run at a time, a second
one is MachineBusy. Sync reads, writes and stimuli work on a parked machine and raise
MachineRunning while it is busy, from any thread. `m.read` and `m.write` over `m.ref` work in
both states, one batch at one block boundary. Handlers and models run on the machine's thread
and access it inline. Inside `running()` a free run goes on while the body awaits or blocks,
`wait_for` waits on it, and what ended it is held: every later wait returns or raises it, and
leaving the body raises it.
"""

from __future__ import annotations

import asyncio
import itertools
import threading
import time

import popcorn
import pytest
from popcorn.units import VirtualTime, ms, us

from .conftest import Machines, StorageModel, run_bounded

NVIC_ISPR0 = 0xE000_E200
HANG = 10.0


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


def test_state_changes_from_another_task_during_a_run_are_machine_running(
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
    start = m.time

    async def scenario() -> int:
        task = asyncio.create_task(m.run())
        (reached,) = await m.read(m.ref.instructions, at=start + us(10))
        task.cancel()
        with pytest.raises(asyncio.CancelledError):
            await task
        assert m.instructions >= reached > 0
        return m.instructions

    cancelled_at = run_bounded(scenario(), HANG)
    stop = asyncio.run(m.run(budget=1000))
    assert stop.reason is popcorn.StopReason.BUDGET
    assert stop.instructions >= cancelled_at + 1000


def test_the_event_loop_keeps_ticking_during_a_run(machines: Machines) -> None:
    """A loop spinning on `asyncio.sleep(0)` never blocks, so a machine thread waiting for the
    GIL would never get it. A machine without Python callbacks needs none: it runs and its wait
    wakes the spinning loop through the loop's eventfd."""
    m = machines(slice_instructions=1_000)
    never = m.ref.mem.u32[m.sym("alu_value")].eq(1)

    async def scenario() -> tuple[popcorn.Stop, int]:
        m.start()
        ticks = 0
        while ticks < 100:
            await asyncio.sleep(0)
            ticks += 1
        waiting = asyncio.create_task(m.wait_for(never, for_=ms(1)))
        while not waiting.done():
            await asyncio.sleep(0)
            ticks += 1
        return await waiting, ticks

    stop, ticks = run_bounded(scenario(), HANG)
    assert m.halt() is None
    assert stop.reason is popcorn.StopReason.TIME
    assert stop.instructions > 0
    assert ticks > 100
    assert m._handle.gil_entries() == 0  # pyright: ignore[reportPrivateUsage]


def test_running_advances_the_machine_in_the_background(machines: Machines) -> None:
    m = machines()
    before = m.instructions
    start = m.time

    async def scenario() -> None:
        reaching = m.read(at=start + us(10))
        async with m.running():
            reached = await reaching
            assert reached.time >= start + us(10)
            with pytest.raises(popcorn.MachineRunning):
                m.reset()

    run_bounded(scenario(), HANG)
    assert m.instructions > before
    assert not m.busy


def test_running_waits_through_idle_without_spending_cpu(machines: Machines) -> None:
    m = machines("idle")
    start = m.time

    async def scenario() -> float:
        reaching = m.read(at=start + us(10))
        async with m.running():
            await reaching
            spent = time.process_time()
            await asyncio.sleep(0.05)
            return time.process_time() - spent

    assert run_bounded(scenario(), HANG) < 0.01
    assert not m.busy


def test_a_fault_inside_running_raises_on_exit(machines: Machines) -> None:
    m = machines("fault")

    async def scenario() -> None:
        async with m.running():
            await m.wait_for(Never())

    with pytest.raises(popcorn.EmulationError) as caught:
        run_bounded(scenario(), HANG)
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

    stop = run_bounded(scenario(), HANG)
    assert stop.reason is popcorn.StopReason.TIME
    assert stop.time >= start + ms(1)


def test_a_body_error_inside_running_carries_the_background_stop(machines: Machines) -> None:
    m = machines("fault")

    async def scenario() -> None:
        async with m.running():
            await m.wait_for(Never())
            raise KeyError("body failed")

    with pytest.raises(KeyError) as caught:
        run_bounded(scenario(), HANG)
    assert any("background run stopped: FAULT" in note for note in caught.value.__notes__)
    assert not m.busy


def test_a_cancelled_running_carries_the_background_stop(machines: Machines) -> None:
    m = machines("fault")

    async def body(faulted: asyncio.Event) -> None:
        async with m.running():
            await m.wait_for(Never())
            faulted.set()
            await asyncio.Event().wait()

    async def scenario() -> asyncio.CancelledError:
        faulted = asyncio.Event()
        task = asyncio.create_task(body(faulted))
        await faulted.wait()
        task.cancel()
        try:
            await task
        except asyncio.CancelledError as cancelled:
            return cancelled
        raise AssertionError("the body was not cancelled")

    cancelled = run_bounded(scenario(), HANG)
    assert any("background run stopped: FAULT" in note for note in cancelled.__notes__)
    assert not m.busy


class AfterInstructions:
    """Ready once the machine has executed a count of instructions."""

    def __init__(self, count: int) -> None:
        self.count = count

    def ready(self, machine: popcorn.Machine) -> bool:
        """Whether the count is reached."""
        return machine.instructions >= self.count


def test_wait_for_inside_running_awaits_the_host(machines: Machines) -> None:
    m = machines(slice_instructions=10_000)

    async def scenario() -> tuple[popcorn.Stop, popcorn.Stop]:
        async with m.running():
            ready = await m.wait_for(AfterInstructions(50_000))
            timed = await m.wait_for(AfterInstructions(10**12), for_=ms(1))
        return ready, timed

    ready, timed = run_bounded(scenario(), HANG)
    assert ready.reason is popcorn.StopReason.USER
    assert 50_000 <= ready.instructions < 50_000 + 10_000 + 64
    assert timed.reason is popcorn.StopReason.TIME


def test_async_time_reads_inside_running_are_monotonic(machines: Machines) -> None:
    m = machines(slice_instructions=10_000)

    async def scenario() -> tuple[list[VirtualTime], popcorn.Stop, VirtualTime]:
        times = [m.time]
        async with m.running():
            for _ in range(50):
                times.append((await m.read()).time)
            stop = await m.wait_for(AfterInstructions(10**12), for_=ms(1))
            at_stop = (await m.read()).time
            with pytest.raises(popcorn.MachineRunning, match=r"await m\.read"):
                await asyncio.to_thread(lambda: m.time)
        return times, stop, at_stop

    times, stop, at_stop = run_bounded(scenario(), HANG)
    assert all(a <= b for a, b in itertools.pairwise(times))
    assert stop.reason is popcorn.StopReason.TIME
    assert at_stop >= stop.time > times[0]
    assert m.time >= at_stop


def test_time_reads_from_another_task_during_a_run(machines: Machines) -> None:
    m = machines(slice_instructions=1_000)

    async def scenario() -> list[VirtualTime]:
        run = asyncio.create_task(m.run(for_=ms(5)))
        times: list[VirtualTime] = []
        while not run.done():
            times.append((await m.read()).time)
            await asyncio.sleep(0)
        stop = await run
        times.append(stop.time)
        return times

    times = run_bounded(scenario(), HANG)
    assert all(a <= b for a, b in itertools.pairwise(times))
    assert m.time == times[-1]


def test_a_sync_read_from_any_thread_during_a_run_is_machine_running(
    machines: Machines,
) -> None:
    m = machines(slice_instructions=1_000)
    started = threading.Event()
    done = threading.Event()

    def predicate(_: popcorn.Machine) -> bool:
        started.set()
        return done.is_set()

    async def scenario() -> popcorn.Stop:
        run = asyncio.create_task(asyncio.to_thread(asyncio.run, m.run(stop_when=predicate)))
        try:
            assert await asyncio.to_thread(started.wait, HANG)
            with pytest.raises(popcorn.MachineRunning, match=r"await m\.read"):
                _ = m.time
        finally:
            done.set()
        return await run

    stop = run_bounded(scenario(), HANG)
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


def test_one_async_read_batch_inside_running_is_consistent(machines: Machines) -> None:
    m = machines(slice_instructions=10_000)
    value = m.var("alu_value")
    address = m.sym("alu_value")

    async def scenario() -> tuple[list[int], list[int], int]:
        values: list[int] = []
        counts: list[int] = []
        async with m.running():
            for _ in range(50):
                sampled, count = await m.read(m.ref.var("alu_value"), m.ref.instructions)
                values.append(int(sampled))
                counts.append(count)
            await m.wait_for(AfterInstructions(counts[-1] + 10_000))
            sampled, count = await m.read(m.ref.var("alu_value"), m.ref.instructions)
            values.append(int(sampled))
            counts.append(count)
            data, word, variable, pc, reg_pc, pending, text = await m.read(
                m.ref.mem.block(address, 4),
                m.ref.mem.u32[address],
                m.ref.var("alu_value"),
                m.ref.pc,
                m.ref.reg.pc,
                m.ref.pending_events,
                m.ref.mem.cstring(address, 4),
            )
            raw = int.from_bytes(data, "little")
            assert reg_pc == pc
            assert pending >= 0
            assert text == data.split(b"\0")[0].decode("utf-8", errors="replace")
            assert raw == word == int(variable)
        return values, counts, m.instructions

    values, counts, after = run_bounded(scenario(), HANG)
    assert values[-1] != values[-2]
    assert counts[-1] >= counts[-2] + 10_000
    assert all(a <= b for a, b in itertools.pairwise(counts))
    assert counts[-1] <= after
    assert value.value == m.var("alu_value").value


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
                with pytest.raises(popcorn.MachineRunning, match=r"await m\.read"):
                    await asyncio.to_thread(read)

    run_bounded(scenario(), HANG)


def test_sync_writes_and_bus_reads_inside_running_raise_and_async_ones_land(
    machines: Machines,
) -> None:
    m = machines(slice_instructions=1_000, models={"TESTDEV": StorageModel})
    value = m.var("alu_value")
    address = m.sym("alu_value")
    testdev = m.description.peripherals[0].base
    sram = next(r for r in m.description.regions if r.name == "SRAM")
    spare = sram.start + sram.size // 2

    refused = r"await m\.read"

    async def scenario() -> None:
        async with m.running():
            with pytest.raises(popcorn.MachineRunning, match=refused):
                m.mem.u32[address] = 1
            with pytest.raises(popcorn.MachineRunning, match=refused):
                m.mem.write(address, b"\0")
            with pytest.raises(popcorn.MachineRunning, match=refused):
                value.value = 1
            with pytest.raises(popcorn.MachineRunning, match=refused):
                m.reg.r0 = 1
            with pytest.raises(popcorn.MachineRunning, match=refused):
                m.periph.TESTDEV.RW.value = 1
            with pytest.raises(popcorn.MachineRunning, match=refused):
                _ = m.mem.u32[testdev]
            with pytest.raises(popcorn.MachineRunning, match=refused):
                _ = m.periph.TESTDEV.RW.read()
            with pytest.raises(popcorn.MachineRunning, match=refused):
                _ = m.nvic.pending(0)
            with pytest.raises(popcorn.MachineRunning, match=refused):
                m.periph.TESTDEV.RW.write(1)
            with pytest.raises(popcorn.MachineRunning, match=refused):
                _ = m.mem.read(sram.start + sram.size - 4, 16)
            written = await m.write(
                (m.ref.mem.u32[address], 1),
                (m.ref.mem.block(address, 1), b"\0"),
                (m.ref.var("alu_value"), 1),
                (m.ref.reg.r0, 1),
                (m.ref.periph.TESTDEV.RW, 1),
                (m.ref.periph.TESTDEV.RW.bus, 1),
                (m.ref.mem.u32[spare], 0x5A5A_A5A5),
            )
            read = await m.read(
                m.ref.mem.u32[testdev],
                m.ref.periph.TESTDEV.RW.bus,
                m.ref.mem.u32[NVIC_ISPR0],
                m.ref.mem.block(sram.start + sram.size - 4, 4),
                m.ref.mem.u32[spare],
            )
            assert len(read) == 5 and read.time >= written.time
            assert read[4] == 0x5A5A_A5A5

    run_bounded(scenario(), HANG)


def test_a_run_inside_running_is_machine_busy(machines: Machines) -> None:
    m = machines()

    async def scenario() -> None:
        async with m.running():
            with pytest.raises(popcorn.MachineBusy):
                await m.run(budget=10)

    run_bounded(scenario(), HANG)


def boom(_: object) -> None:
    raise ValueError("handler failed")


class Recorded:
    """A handler raising ValueError, telling the awaiting loop it ran."""

    def __init__(self) -> None:
        self.ran: asyncio.Event | None = None
        self.loop: asyncio.AbstractEventLoop | None = None

    def arm(self) -> asyncio.Event:
        """Bind to the running loop, the event set once the handler ran."""
        self.ran = asyncio.Event()
        self.loop = asyncio.get_running_loop()
        return self.ran

    def __call__(self, _: object) -> None:
        """Signal the loop and fail."""
        if self.loop is not None and self.ran is not None:
            self.loop.call_soon_threadsafe(self.ran.set)
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
        run_bounded(scenario(), HANG)
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
        run_bounded(scenario(), HANG)
    assert not m.busy


def test_a_watch_error_inside_running_raises_at_the_next_wait(machines: Machines) -> None:
    m = machines(slice_instructions=1_000)
    m.watch("alu_rounds", handler=boom)

    async def scenario() -> None:
        async with m.running():
            await m.wait_for(popcorn.Reached("idle_main"), for_=ms(100))

    with pytest.raises(ValueError, match="handler failed"):
        run_bounded(scenario(), HANG)
    assert not m.busy


def test_a_handler_error_inside_running_raises_on_exit_without_a_wait(
    machines: Machines,
) -> None:
    m = machines(slice_instructions=1_000)
    handler = Recorded()
    m.on_return("alu_mark", handler)
    instructions: list[int] = []

    async def scenario() -> None:
        ran = handler.arm()
        async with m.running():
            await ran.wait()
            instructions.append((await m.read(m.ref.instructions))[0])
            await asyncio.sleep(0.01)
            instructions.append((await m.read(m.ref.instructions))[0])

    with pytest.raises(ValueError, match="handler failed"):
        run_bounded(scenario(), HANG)
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
        run_bounded(scenario(), HANG)
    assert errors == [caught.value, caught.value]
    assert not m.busy


def test_a_body_error_inside_running_carries_the_handler_error(machines: Machines) -> None:
    m = machines(slice_instructions=1_000)
    handler = Recorded()
    m.on_return("alu_mark", handler)

    async def scenario() -> None:
        ran = handler.arm()
        async with m.running():
            await ran.wait()
            raise KeyError("body failed")

    with pytest.raises(KeyError) as caught:
        run_bounded(scenario(), HANG)
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

    assert run_bounded(scenario(), HANG).reason is popcorn.StopReason.USER


def test_a_wait_after_the_background_run_stopped_returns_that_stop(machines: Machines) -> None:
    m = machines("fault")
    stops: list[popcorn.Stop] = []

    async def scenario() -> None:
        async with m.running():
            stops.append(await m.wait_for(Never()))
            stops.append(await m.wait_for(Never(), for_=ms(1)))

    with pytest.raises(popcorn.FaultError):
        run_bounded(scenario(), HANG)
    assert stops[0] == stops[1]
    assert stops[0].reason is popcorn.StopReason.FAULT
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
        run_bounded(scenario(), HANG)
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
        run_bounded(scenario(), HANG)
    assert not m.busy


def test_a_machine_runs_again_after_a_handler_error_inside_running(machines: Machines) -> None:
    m = machines(slice_instructions=1_000)
    saved = m.snapshot()
    hook = m.on_return("alu_mark", boom)

    async def failing() -> None:
        async with m.running():
            await m.wait_for(Never(), for_=ms(100))

    async def passing() -> popcorn.Stop:
        target = AfterInstructions(m.instructions + 50_000)
        async with m.running():
            return await m.wait_for(target, for_=ms(100))

    with pytest.raises(ValueError, match="handler failed"):
        run_bounded(failing(), HANG)
    hook.remove()
    assert run_bounded(passing(), HANG).reason is popcorn.StopReason.USER

    m.on_return("alu_mark", boom)
    m.restore(saved)
    with pytest.raises(ValueError, match="handler failed"):
        run_bounded(failing(), HANG)
    m.remove_hooks()
    m.restore(saved)
    assert run_bounded(passing(), HANG).reason is popcorn.StopReason.USER
    assert not m.busy
