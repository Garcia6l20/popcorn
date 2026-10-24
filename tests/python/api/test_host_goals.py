"""The goals of the machine host: the machine runs while a sync body blocks, awaiting firmware
waits on the machine's thread without turning the event loop, and the host's edges around it."""

from __future__ import annotations

import asyncio
import contextlib
import contextvars
import functools
import gc
import inspect
import itertools
import pathlib
import selectors
import socket
import subprocess
import sys
import textwrap
import threading
import time
import traceback
import types
import weakref
from collections.abc import Callable, Coroutine
from typing import NoReturn, cast

import popcorn
import pytest
from popcorn import _core
from popcorn._host import awaited
from popcorn.trace import TraceKind
from popcorn.trace import read as read_trace
from popcorn.units import VirtualDuration, VirtualTime, ms, s, us

from .conftest import (
    CALLEE_ELF,
    DRIVER_ELF,
    M33_JSON,
    REFUSED_INSIDE,
    REQUESTS_INSIDE,
    VIEWS_ELF,
    Machines,
    requests_inside,
    run_bounded,
    stalled_until_posted,
)
from .models_example import Timer
from .projects import MODEL_ELF, MODEL_JSON

HANG = 10.0


class Never:
    """Never ready."""

    def ready(self, machine: popcorn.Machine) -> bool:
        """Always False."""
        return False


class AfterInstructions:
    """Ready once the machine has executed a count of instructions."""

    def __init__(self, count: int) -> None:
        self.count = count

    def ready(self, machine: popcorn.Machine) -> bool:
        """Whether the count is reached."""
        return machine.instructions >= self.count


class AfterMarks:
    """Ready once the firmware called `alu_mark` a count of times."""

    def __init__(self, count: int) -> None:
        self.count = count

    def ready(self, machine: popcorn.Machine) -> bool:
        """Whether `alu_rounds` reached the count."""
        return int(machine.var("alu_rounds").value) >= self.count


class CountingSelector(selectors.DefaultSelector):
    """Counts the event loop's turns, one `select` call each."""

    def __init__(self) -> None:
        super().__init__()
        self.calls = 0

    def select(self, timeout: float | None = None) -> list[tuple[selectors.SelectorKey, int]]:
        """Count the call and select."""
        self.calls += 1
        return super().select(timeout)


def counted[T](scenario: Callable[[CountingSelector], Coroutine[object, object, T]]) -> T:
    """Run `scenario` on an event loop whose selector it can read."""
    selector = CountingSelector()
    return asyncio.run(scenario(selector), loop_factory=lambda: asyncio.SelectorEventLoop(selector))


def block_on_sleep(reached: threading.Event, _: socket.socket) -> None:
    deadline = time.monotonic() + HANG
    while not reached.is_set() and time.monotonic() < deadline:
        time.sleep(0.001)


def block_on_socket(_: threading.Event, reader: socket.socket) -> None:
    reader.settimeout(HANG)
    assert reader.recv(1) == b"!"


def block_on_event(reached: threading.Event, _: socket.socket) -> None:
    reached.wait(HANG)


@pytest.mark.parametrize(
    "block",
    [block_on_sleep, block_on_socket, block_on_event],
    ids=["time-sleep", "blocking-socket", "threading-wait"],
)
def test_the_machine_runs_while_a_sync_body_blocks(
    machines: Machines, block: Callable[[threading.Event, socket.socket], None]
) -> None:
    m = machines(slice_instructions=1_000)
    before = m.instructions
    reached = threading.Event()
    reader, writer = socket.socketpair()
    rounds: list[int] = []

    def counting(_: popcorn.WatchHit) -> None:
        rounds.append(threading.get_ident())
        if len(rounds) == 1_000:
            writer.send(b"!")
            reached.set()

    m.watch("alu_value", handler=counting)
    m.start()
    try:
        block(reached, reader)
        with pytest.raises(popcorn.MachineRunning):
            _ = m.instructions
    finally:
        m.halt()
        reader.close()
        writer.close()
    assert reached.is_set()
    assert threading.get_ident() not in rounds
    assert not m.busy
    assert m.instructions > before + 1_000


def test_awaiting_a_firmware_event_does_not_turn_the_event_loop(machines: Machines) -> None:
    m = machines(slice_instructions=1_000)
    rounds = 3_000_000

    async def scenario(selector: CountingSelector) -> tuple[int, int, popcorn.Stop, popcorn.Stop]:
        first = await m.wait_for(popcorn.Reached("alu_mark"))
        m.reg.r4 = (1_000 - rounds) % 2**32
        before = selector.calls
        again = await m.wait_for(popcorn.Reached("alu_mark"))
        run_calls = selector.calls - before
        m.reg.r4 = (1_000 - rounds) % 2**32
        async with m.running():
            before = selector.calls
            waited = await m.wait_for(AfterMarks(3))
            wait_calls = selector.calls - before
        assert first.reason is popcorn.StopReason.ADDRESS
        return run_calls, wait_calls, again, waited

    run_calls, wait_calls, again, waited = counted(scenario)
    assert again.reason is popcorn.StopReason.ADDRESS
    assert again.instructions > rounds * 5
    assert waited.reason is popcorn.StopReason.USER
    assert waited.instructions > again.instructions + rounds * 5
    assert run_calls < 10
    assert wait_calls < 10


def test_cancelling_a_wait_keeps_the_machine_running(machines: Machines) -> None:
    m = machines(slice_instructions=1_000)

    async def scenario() -> tuple[int, popcorn.Stop]:
        async with m.running():
            waiting = asyncio.create_task(m.wait_for(Never()))
            await asyncio.sleep(0)
            waiting.cancel()
            with pytest.raises(asyncio.CancelledError):
                await waiting
            assert m.busy
            (count,) = await m.read(m.ref.instructions)
            return count, await m.wait_for(AfterInstructions(count + 100_000))

    count, stop = run_bounded(scenario(), HANG)
    assert stop.reason is popcorn.StopReason.USER
    assert stop.instructions >= count + 100_000
    assert not m.busy


def test_cancelling_a_run_parks_the_machine_at_once(machines: Machines) -> None:
    m = machines()
    start = m.time

    async def scenario() -> int:
        run = asyncio.create_task(m.run())
        (count,) = await m.read(m.ref.instructions, at=start + us(10))
        run.cancel()
        with pytest.raises(asyncio.CancelledError):
            await run
        assert not m.busy
        assert m.instructions >= count > 0
        return count

    run_bounded(scenario(), HANG)


def stamped_state(machines: Machines, *, write: bool) -> tuple[VirtualTime, VirtualTime, int, int]:
    m = machines()
    start = m.time
    written = m.write((m.ref.reg.r5, 0x1234_5678), at=start + us(500)) if write else None
    read = m.read(m.ref.var("alu_value"), m.ref.instructions, at=start + us(1_000))

    async def scenario() -> tuple[VirtualTime, VirtualTime, int, int]:
        async with m.running():
            at = start + us(500) if written is None else (await written).time
            values = await read
        return at, values.time, int(values[0]), values[1]

    state = run_bounded(scenario(), HANG)
    assert state[0] >= start + us(500)
    assert state[1] >= start + us(1_000)
    return state


def test_a_stamped_write_inside_running_gives_the_same_state_on_two_machines(
    machines: Machines,
) -> None:
    first = stamped_state(machines, write=True)
    second = stamped_state(machines, write=True)
    unwritten = stamped_state(machines, write=False)
    assert first == second
    assert first[1:] != unwritten[1:]


def test_halt_is_refused_from_a_handler_and_during_a_run(machines: Machines) -> None:
    m = machines(slice_instructions=1_000)
    errors: list[BaseException] = []

    def halting(machine: popcorn.Machine) -> bool:
        try:
            machine.halt()
        except RuntimeError as error:
            errors.append(error)
        return True

    asyncio.run(m.run(stop_when=halting))
    assert len(errors) == 1

    async def scenario() -> None:
        run = asyncio.create_task(m.run())
        await asyncio.sleep(0)
        with pytest.raises(popcorn.MachineBusy, match="cancel its task"):
            m.halt()
        run.cancel()
        with pytest.raises(asyncio.CancelledError):
            await run

    run_bounded(scenario(), HANG)
    assert not m.busy


def test_a_started_machine_dropped_without_halt_joins_its_thread(machines: Machines) -> None:
    gc.collect()
    before = _core.host_threads()
    m = machines()
    m.start()
    del m
    gc.collect()
    assert _core.host_threads() == before


def started_and_ended_by_a_handler(
    machines: Machines, surfaced: bool
) -> weakref.ref[popcorn.Machine]:
    m = machines(slice_instructions=1_000)

    def failing(_: object) -> None:
        raise ValueError(f"a handler of {m!r} failed")

    m.on_return("alu_mark", failing)
    m.start()
    if surfaced:
        with pytest.raises(ValueError, match="a handler of"):
            run_bounded(m.wait_for(Never()), HANG)
    else:
        ended = threading.Event()
        end = m._handle.wait(None, None, None)  # pyright: ignore[reportPrivateUsage]
        end.add_done_callback(ended.set)
        assert ended.wait(HANG)
    return weakref.ref(m)


@pytest.mark.parametrize("surfaced", [False, True], ids=["unclaimed", "surfaced"])
def test_a_machine_whose_free_run_a_handler_ended_is_collected(
    machines: Machines, monkeypatch: pytest.MonkeyPatch, surfaced: bool
) -> None:
    reported: list[str] = []

    def report(raised: sys.UnraisableHookArgs) -> None:
        reported.append(repr(raised.exc_value))

    monkeypatch.setattr(sys, "unraisablehook", report)
    gc.collect()
    before = _core.host_threads()
    probe = started_and_ended_by_a_handler(machines, surfaced)
    gc.collect()
    assert probe() is None
    assert _core.host_threads() == before
    if surfaced:
        assert reported == []
    else:
        [unclaimed] = reported
        assert unclaimed.startswith("EmulationError(") and "collected" in unclaimed
        assert "ValueError: a handler of" in unclaimed


def raised_in_handler(machines: Machines) -> weakref.ref[popcorn.Machine]:
    """Start a machine whose intercept raises, return once the handler began."""
    m = machines()
    ended = threading.Event()

    def handler(_: object) -> NoReturn:
        ended.set()
        raise ValueError("boom")

    m.intercept("alu_mark", handler)
    m.start()
    assert ended.wait(HANG)
    return weakref.ref(m)


@pytest.mark.timeout(60)
def test_a_single_collection_right_after_a_handler_raised_collects_its_machine(
    machines: Machines, monkeypatch: pytest.MonkeyPatch
) -> None:
    reported: list[str] = []

    def report(raised: sys.UnraisableHookArgs) -> None:
        reported.append(repr(raised.exc_value))

    monkeypatch.setattr(sys, "unraisablehook", report)
    interval = sys.getswitchinterval()
    enabled = gc.isenabled()
    sys.setswitchinterval(1.0)
    gc.disable()
    try:
        for _ in range(200):
            reported.clear()
            probe = raised_in_handler(machines)
            gc.collect()
            assert probe() is None
            assert len(reported) == 1 and "boom" in reported[0], reported
    finally:
        sys.setswitchinterval(interval)
        if enabled:
            gc.enable()


def raised_by(error: BaseException, name: str) -> types.FrameType:
    """The frame of the traceback entry of `error` running the function `name`."""
    found = [
        frame for frame, _ in traceback.walk_tb(error.__traceback__) if frame.f_code.co_name == name
    ]
    assert found, f"no frame of {name}"
    return found[-1]


def test_a_handler_error_keeps_its_traceback_lines_without_frame_locals(
    machines: Machines,
) -> None:
    m = machines(slice_instructions=1_000)

    def failing_handler(_: object) -> None:
        payload = Payload()
        raise ValueError(f"boom {payload!r}")

    m.on_return("alu_mark", failing_handler)
    m.start()
    with pytest.raises(ValueError, match="boom") as raised:
        run_bounded(m.wait_for(Never()), HANG)
    with pytest.raises(ValueError, match="boom"):
        m.halt()
    text = "".join(traceback.format_exception(raised.value))
    assert "in failing_handler" in text
    assert 'raise ValueError(f"boom {payload!r}")' in text
    assert dict(raised_by(raised.value, "failing_handler").f_locals) == {}


def test_a_chained_exception_whose_frame_still_runs_keeps_its_traceback(
    machines: Machines,
) -> None:
    m = machines(slice_instructions=1_000)

    def helper() -> None:
        marker = "kept"
        raise KeyError(marker)

    try:
        helper()
    except KeyError as caught:
        stored = caught

        def chaining(_: object) -> None:
            raise ValueError("boom") from stored

        m.on_return("alu_mark", chaining)
        m.start()
        with pytest.raises(ValueError, match="boom") as raised:
            run_bounded(m.wait_for(Never()), HANG)
        with pytest.raises(ValueError, match="boom"):
            m.halt()
        assert raised.value.__cause__ is stored
        assert stored.__traceback__ is not None
        assert stored.__traceback__.tb_frame is inspect.currentframe()
        assert stored.__traceback__.tb_next is not None
        assert stored.__traceback__.tb_next.tb_frame.f_locals["marker"] == "kept"


def failed_command(machines: Machines) -> weakref.ref[popcorn.Machine]:
    """Run a command whose body raises on the machine's thread and check the error's body frame
    has no locals. The future is passed inline: a local holding it in a frame the error passes
    through would close a cycle through the native future the collector cannot see."""
    m = machines(slice_instructions=1_000)

    def failing_body() -> None:
        payload = Payload()
        raise ValueError(f"boom {payload!r}")

    async def scenario() -> None:
        async with m.running():
            handle = m._handle  # pyright: ignore[reportPrivateUsage]
            await awaited(handle.submit(failing_body, None), lambda raw: raw)

    with pytest.raises(ValueError, match="boom") as raised:
        run_bounded(scenario(), HANG)
    assert dict(raised_by(raised.value, "failing_body").f_locals) == {}
    return weakref.ref(m)


def test_a_command_error_on_the_machine_thread_drops_its_frame_locals_and_its_machine(
    machines: Machines,
) -> None:
    probe = failed_command(machines)
    gc.collect()
    assert probe() is None


def test_a_machine_with_a_python_model_runs_under_a_loop_that_yields_for_real() -> None:
    """Its model needs the GIL on the machine's thread: a loop spinning on `asyncio.sleep(0)`
    can starve it, one sleeping 1 ms between turns lets it run."""
    m = popcorn.Machine(
        popcorn.load_description(MODEL_JSON),
        image=popcorn.Image.elf(MODEL_ELF),
        models={"TIMER": Timer},
    )
    m.reset()
    asyncio.run(m.run(until=m.sym("ready"), budget=10_000))
    asyncio.run(m.call("start_timer", 10, budget=10_000))
    irqs = m.ref.mem.u32[m.sym("timer_irqs")].ge(3)

    async def scenario() -> popcorn.Stop:
        m.start()
        waiting = asyncio.create_task(m.wait_for(irqs))
        while not waiting.done():
            await asyncio.sleep(0.001)
        return await waiting

    stop = run_bounded(scenario(), HANG)
    assert m.halt() is None
    assert stop.reason is popcorn.StopReason.USER
    assert m._handle.gil_entries() > 0  # pyright: ignore[reportPrivateUsage]


def test_a_trace_close_refused_inside_another_machine_keeps_its_sink(
    machines: Machines, tmp_path: pathlib.Path
) -> None:
    traced, other = machines(), machines()
    path = tmp_path / "t.pctr"
    tracing = traced.trace([TraceKind.BLOCK], path)
    recording = tracing.__enter__()
    traced.start()

    def leave() -> None:
        tracing.__exit__(None, None, None)

    with pytest.raises(RuntimeError, match="handler accesses the machine"):
        other._handle.parked(leave)  # pyright: ignore[reportPrivateUsage]
    assert traced.halt() is None
    grew = recording.records
    asyncio.run(traced.run(for_=us(20)))
    assert recording.records > grew
    recording.close()
    closed = recording.records
    asyncio.run(traced.run(for_=us(20)))
    assert recording.records == closed
    assert sum(1 for _ in read_trace(path)) > 0


def test_a_reached_wait_in_a_free_run_holds_no_gil(m33: popcorn.PartDescription) -> None:
    m = popcorn.Machine(m33, image=popcorn.Image.elf(VIEWS_ELF))
    m.reset()
    assert asyncio.run(m.run(budget=100_000)).reason is popcorn.StopReason.USER
    assert asyncio.run(m.run(budget=1_000)).reason is popcorn.StopReason.BUDGET
    loop = m.pc

    async def scenario() -> popcorn.Stop:
        async with m.running():
            return await m.wait_for(popcorn.Reached(loop), for_=ms(1))

    stop = run_bounded(scenario(), HANG)
    assert stop.reason is popcorn.StopReason.USER and stop.pc == loop
    assert m._handle.gil_entries() == 0  # pyright: ignore[reportPrivateUsage]


class Payload:
    """A local whose life shows whether a frame is still held."""


def boom(_: object) -> None:
    raise ValueError("boom")


def caught_and_halted(m: popcorn.Machine) -> weakref.ref[Payload]:
    payload = Payload()
    m.start()
    with pytest.raises(ValueError, match="boom"):
        run_bounded(m.wait_for(Never()), HANG)
    with pytest.raises(ValueError, match="boom"):
        m.halt()
    return weakref.ref(payload)


def test_a_halted_handler_error_keeps_no_frame_alive_in_a_kept_machine(
    machines: Machines,
) -> None:
    m = machines(slice_instructions=1_000)
    m.on_return("alu_mark", boom)
    probe = caught_and_halted(m)
    gc.collect()
    assert probe() is None
    assert not m.busy


class StalledWait:
    """A free run wait whose completion wakes its loop, then stalls the machine's thread until
    `release` is set or `seconds` elapse."""

    def __init__(self, seconds: float) -> None:
        self.seconds = seconds
        self.stalled = threading.Event()
        self.release = threading.Event()
        self.resumed = threading.Event()

    async def wait(self, m: popcorn.Machine) -> None:
        """Wait on the free run of `m` until it ends, the machine's thread stalled after."""
        handle = m._handle  # pyright: ignore[reportPrivateUsage]
        future = handle.wait(functools.partial(Never().ready, m), None, None)
        loop = asyncio.get_running_loop()
        woken = loop.create_future()

        def wake_then_stall() -> None:
            self.stalled.set()
            loop.call_soon_threadsafe(woken.set_result, None)
            self.release.wait(self.seconds)
            self.resumed.set()

        future.add_done_callback(wake_then_stall)
        await woken
        try:
            future.result(0.0)
        finally:
            del future


def ended_while_stalled(machines: Machines, stall: StalledWait) -> weakref.ref[popcorn.Machine]:
    m = machines(slice_instructions=1_000)
    m.on_return("alu_mark", boom)
    m.start()
    with pytest.raises(ValueError, match="boom"):
        run_bounded(stall.wait(m), HANG)
    return weakref.ref(m, lambda _: stall.release.set())


def test_a_wait_ended_by_a_handler_holds_no_machine_once_it_completes(
    machines: Machines, monkeypatch: pytest.MonkeyPatch
) -> None:
    reported: list[str] = []

    def report(raised: sys.UnraisableHookArgs) -> None:
        reported.append(repr(raised.exc_value))

    monkeypatch.setattr(sys, "unraisablehook", report)
    stall = StalledWait(HANG)
    probe = ended_while_stalled(machines, stall)
    try:
        assert stall.stalled.is_set()
        gc.collect()
        assert probe() is None
    finally:
        stall.release.set()
    assert reported == []


def halted_while_stalled(m: popcorn.Machine, stall: StalledWait) -> weakref.ref[Payload]:
    payload = Payload()
    m.start()
    with pytest.raises(ValueError, match="boom"):
        run_bounded(stall.wait(m), HANG)
    with pytest.raises(ValueError, match="boom"):
        m.halt()
    return weakref.ref(payload)


def test_halt_returns_once_the_machine_thread_released_the_end(machines: Machines) -> None:
    """The machine thread stalls 1 s while it still holds the end: a halt returning before the
    release would find the end alive. A shorter stall could end under load before the halt even
    started waiting, and pass without testing the wait."""
    m = machines(slice_instructions=1_000)
    m.on_return("alu_mark", boom)
    stall = StalledWait(1.0)
    probe = halted_while_stalled(m, stall)
    assert stall.resumed.is_set()
    gc.collect()
    assert probe() is None


def test_a_wait_completed_by_one_end_raises_that_end_after_a_later_one(
    machines: Machines,
) -> None:
    m = machines(slice_instructions=1_000)
    calls = itertools.count(1)

    def numbered(_: object) -> None:
        raise ValueError(f"end {next(calls)}")

    m.on_return("alu_mark", numbered)
    later: list[str] = []

    def wait_on_another_thread() -> None:
        try:
            asyncio.run(m.wait_for(Never()))
        except ValueError as error:
            later.append(str(error))

    async def first() -> None:
        with pytest.raises(ValueError, match="end 1"):
            await m.wait_for(Never())
        with pytest.raises(ValueError, match="end 1"):
            m.halt()
        m.reg.r4 = 0
        m.start()
        waiter = threading.Thread(target=wait_on_another_thread)
        waiter.start()
        waiter.join(HANG)

    async def other() -> str:
        try:
            await m.wait_for(Never())
        except ValueError as error:
            return str(error)
        return "no error"

    async def scenario() -> str:
        m.start()
        _, seen = await asyncio.gather(first(), other())
        return seen

    assert run_bounded(scenario(), HANG) == "end 1"
    assert later == ["end 2"]
    with pytest.raises(ValueError, match="end 2"):
        m.halt()


def test_an_interpreter_exits_with_a_machine_started_and_never_halted() -> None:
    child = subprocess.run(
        [
            sys.executable,
            "-c",
            textwrap.dedent(f"""
                import popcorn

                m = popcorn.Machine(
                    popcorn.load_description({str(M33_JSON)!r}),
                    image=popcorn.Image.elf({str(DRIVER_ELF)!r}),
                )
                m.reset()
                m.start()
                """),
        ],
        capture_output=True,
        text=True,
        timeout=HANG,
        check=False,
    )
    assert child.returncode == 0, child.stderr


def test_a_busy_python_thread_does_not_change_what_a_wait_returns(machines: Machines) -> None:
    m = machines(slice_instructions=1_000)
    done = threading.Event()

    def spin() -> None:
        count = 0
        while not done.is_set():
            count += 1

    async def scenario() -> tuple[popcorn.Stop, popcorn.Stop]:
        reached = await m.wait_for(popcorn.Reached("alu_mark"))
        async with m.running():
            marked = await m.wait_for(AfterMarks(1))
        return reached, marked

    spinner = threading.Thread(target=spin)
    spinner.start()
    try:
        reached, marked = run_bounded(scenario(), HANG)
    finally:
        done.set()
        spinner.join()
    assert reached.reason is popcorn.StopReason.ADDRESS
    assert reached.pc == m.sym("alu_mark")
    assert marked.reason is popcorn.StopReason.USER
    assert int(m.var("alu_rounds").value) == 1


SLICE = 20_000


class Counting:
    """Never ready, counting its calls."""

    def __init__(self) -> None:
        self.calls = 0

    def ready(self, machine: popcorn.Machine) -> bool:
        """Count the call, return False."""
        self.calls += 1
        return False


def ticking(m33: popcorn.PartDescription) -> popcorn.Machine:
    """A reset machine over the callee firmware, its SysTick counting `ticks`, whose only slice
    ends are its waiters' deadlines."""
    m = popcorn.Machine(
        m33,
        image=popcorn.Image.elf(CALLEE_ELF),
        slice_instructions=SLICE,
        slice_wall_budget=10.0,
    )
    m.reset()
    return m


class Gate:
    """Holds a machine's thread at its first write to `ticks`, inside a slice, until opened."""

    def __init__(self, m: popcorn.Machine) -> None:
        """Watch `ticks` on the parked `m`."""
        self.entered = threading.Event()
        self.opened = threading.Event()
        m.watch("ticks", handler=self.hold)

    def hold(self, hit: popcorn.WatchHit) -> None:
        """Signal the hold, wait until opened."""
        self.entered.set()
        self.opened.wait(HANG)


def gated(m: popcorn.Machine, event: popcorn.Event, for_: VirtualDuration) -> popcorn.Stop:
    """`wait_for(event)` inside `running()`, posted while the machine's thread is held at its
    first write to `ticks`, so the wait exists before `ticks` passes 1."""
    gate = Gate(m)

    async def scenario() -> popcorn.Stop:
        async with m.running():
            assert await asyncio.to_thread(gate.entered.wait, HANG), "no write to ticks"
            waiting = asyncio.ensure_future(m.wait_for(event, for_=for_))
            await asyncio.sleep(0)
            gate.opened.set()
            return await waiting

    return run_bounded(scenario(), HANG)


def test_a_python_wait_posted_inside_a_slice_counts_for_from_its_post(
    m33: popcorn.PartDescription,
) -> None:
    m = popcorn.Machine(
        m33,
        image=popcorn.Image.elf(CALLEE_ELF),
        slice_instructions=100_000_000,
        slice_wall_budget=10.0,
    )
    m.reset()
    stop = gated(m, Never(), ms(1))
    assert stop.reason is popcorn.StopReason.TIME
    assert ms(1).ps < stop.time.ps < ms(3).ps


class Recording:
    """Always ready, recording `m.instructions` at each check."""

    def __init__(self) -> None:
        self.calls: list[int] = []

    def ready(self, machine: popcorn.Machine) -> bool:
        """Record the check, return True."""
        self.calls.append(machine.instructions)
        return True


def test_a_python_wait_is_not_checked_at_the_boundary_a_sibling_post_creates(
    m33: popcorn.PartDescription,
) -> None:
    m = popcorn.Machine(
        m33,
        image=popcorn.Image.elf(CALLEE_ELF),
        slice_instructions=100_000_000,
        slice_wall_budget=10.0,
    )
    m.reset()
    gate = Gate(m)
    event = Recording()

    async def scenario() -> popcorn.Stop:
        async with m.running():
            assert await asyncio.to_thread(gate.entered.wait, HANG), "no write to ticks"
            first = asyncio.ensure_future(m.wait_for(event))
            sibling = asyncio.ensure_future(m.wait_for(Never(), for_=ms(50)))
            await asyncio.sleep(0)
            gate.opened.set()
            await sibling
            return await first

    stop = run_bounded(scenario(), HANG)
    assert stop.reason is popcorn.StopReason.USER
    assert event.calls[0] > 1_000, event.calls


def test_a_free_run_waiter_predicate_runs_once_per_slice(m33: popcorn.PartDescription) -> None:
    m = ticking(m33)
    event = Counting()
    before = m.instructions

    async def scenario() -> popcorn.Stop:
        async with m.running():
            return await m.wait_for(event, for_=ms(50))

    stop = run_bounded(scenario(), HANG)
    assert stop.reason is popcorn.StopReason.TIME
    executed = m.instructions - before
    events = int(m.var("ticks").value)
    assert events > 100
    assert event.calls <= executed // SLICE + 2
    assert event.calls < events // 10


def test_a_python_busy_thread_slows_a_free_run_waiter_per_slice_only(
    m33: popcorn.PartDescription,
) -> None:
    m = ticking(m33)
    done = threading.Event()

    def spin() -> None:
        count = 0
        while not done.is_set():
            count += 1

    async def scenario() -> popcorn.Stop:
        await m.run(budget=10)
        async with m.running():
            return await m.wait_for(Never(), for_=ms(100))

    spinner = threading.Thread(target=spin)
    spinner.start()
    try:
        stop = run_bounded(scenario(), 6 * HANG)
    finally:
        done.set()
        spinner.join()
    assert stop.reason is popcorn.StopReason.TIME


EXACT = 100_000_000
SRAM = 0x2000_8000


def exact(m33: popcorn.PartDescription, elf: pathlib.Path = CALLEE_ELF) -> popcorn.Machine:
    """A reset machine whose only slice ends are its waiters' deadlines and posted requests."""
    m = popcorn.Machine(
        m33, image=popcorn.Image.elf(elf), slice_instructions=EXACT, slice_wall_budget=10.0
    )
    m.reset()
    return m


def waited(m: popcorn.Machine, event: popcorn.Event, for_: VirtualDuration) -> popcorn.Stop:
    """`wait_for(event)` inside `running()`."""

    async def scenario() -> popcorn.Stop:
        async with m.running():
            return await m.wait_for(event, for_=for_)

    return run_bounded(scenario(), HANG)


def test_a_condition_fires_where_a_parked_predicate_does(m33: popcorn.PartDescription) -> None:
    parked, running = exact(m33), exact(m33)
    Gate(parked).opened.set()
    want = asyncio.run(parked.run(for_=ms(200), stop_when=lambda m: m.var("ticks").value >= 100))
    condition = running.ref.var("ticks").ge(100)
    got = gated(running, condition, ms(200))
    assert want.reason is popcorn.StopReason.USER
    assert got.reason is popcorn.StopReason.USER
    assert (got.time, got.pc, got.instructions) == (want.time, want.pc, want.instructions)
    assert got.fired == (condition,)


def stop_on_tick(m: popcorn.Machine) -> None:
    m.intercept("SysTick_Handler", lambda call: call.stop())


def test_a_condition_reports_itself_in_stop_fired_parked(m33: popcorn.PartDescription) -> None:
    m = exact(m33)
    fires, never = m.ref.var("ticks").ge(3), m.ref.var("ticks").gt(1_000_000)
    fired = asyncio.run(m.wait_for(fires, for_=ms(10)))
    assert fired.reason is popcorn.StopReason.USER
    assert fired.fired == (fires,)
    assert m.last_stop is not None and m.last_stop.fired == (fires,)
    timed = asyncio.run(m.wait_for(never, for_=ms(10)))
    assert timed.reason is popcorn.StopReason.TIME
    assert timed.fired == ()
    stop_on_tick(m)
    stopped = asyncio.run(m.wait_for(never, for_=ms(10)))
    assert stopped.reason is popcorn.StopReason.USER
    assert stopped.fired == ()


def test_a_parked_condition_s_fired_is_taken_at_its_stopping_boundary(
    m33: popcorn.PartDescription,
) -> None:
    m = exact(m33)
    ticks = m.ref.var("ticks")
    fires, later = ticks.ge(3), ticks.ge(1_000)
    written: list[bool] = []

    async def scenario() -> popcorn.Stop:
        waiting = asyncio.create_task(m.wait_for(popcorn.any_of(fires, later), for_=ms(10)))
        await asyncio.sleep(0)
        while m.busy:
            await asyncio.sleep(0)
        written.append(not waiting.done())
        m.var("ticks").value = 2_000
        return await waiting

    stop = run_bounded(scenario(), HANG)
    assert written == [True]
    assert stop.reason is popcorn.StopReason.USER
    assert stop.fired == (fires,)
    assert later.ready(m)


def test_a_condition_reports_itself_in_stop_fired_running(m33: popcorn.PartDescription) -> None:
    m = exact(m33)
    fires, never = m.ref.var("ticks").ge(3), m.ref.var("ticks").gt(1_000_000)
    fired = waited(m, fires, ms(10))
    assert fired.reason is popcorn.StopReason.USER
    assert fired.fired == (fires,)
    timed = waited(m, never, ms(10))
    assert timed.reason is popcorn.StopReason.TIME
    assert timed.fired == ()
    stop_on_tick(m)
    stopped = asyncio.run(collect_stop(m, never))
    assert stopped.reason is popcorn.StopReason.USER
    assert stopped.fired == ()


async def collect_stop(m: popcorn.Machine, condition: popcorn.Condition) -> popcorn.Stop:
    """Wait on `condition` in a free run a handler's `stop()` ends, then halt quietly."""
    m.start()
    try:
        return await m.wait_for(condition, for_=ms(10))
    finally:
        with contextlib.suppress(popcorn.EmulationError):
            m.halt()


def test_a_condition_already_true_fires_at_once(m33: popcorn.PartDescription) -> None:
    m = exact(m33)
    condition = m.ref.var("ticks").ge(0)
    stop = waited(m, condition, s(5))
    assert stop.reason is popcorn.StopReason.USER
    assert stop.fired == (condition,)
    assert stop.instructions < EXACT // 100


def views_machine(m33: popcorn.PartDescription) -> popcorn.Machine:
    """A machine over the views firmware past its bkpt, counting in a loop."""
    m = exact(m33, VIEWS_ELF)
    assert asyncio.run(m.run(budget=100_000)).reason is popcorn.StopReason.USER
    return m


def holds_both_ways(m: popcorn.Machine, condition: popcorn.Condition) -> bool:
    """Whether `condition` holds through its parked `ready` and natively, which must agree."""
    parked = condition.ready(m)
    running = waited(m, condition, us(10))
    assert running.fired == ((condition,) if parked else ()), (condition, parked, running)
    return parked


BUILT: list[tuple[Callable[[popcorn.Machine], popcorn.Condition], bool]] = [
    (lambda m: m.ref.var("table").delta.lt(0), True),
    (lambda m: m.ref.var("table").delta.eq(-3), True),
    (lambda m: m.ref.var("table").delta.lt(-3), False),
    (lambda m: m.ref.var("table").mode.eq(17), True),
    (lambda m: m.ref.var("wide").mid.eq(0xABC), True),
    (lambda m: m.ref.var("wide").mid.gt(0xABC), False),
    (lambda m: m.ref.var("wide").high.lt(0), True),
    (lambda m: m.ref.var("wide").high.ge(-5), True),
    (lambda m: m.ref.var("extras").low.eq(-2), True),
    (lambda m: m.ref.var("table").offset.le(-300), True),
    (lambda m: m.ref.var("table").scale.eq(2.25), True),
    (lambda m: m.ref.var("table").entries[1].gain.gt(1.4), True),
    (lambda m: m.ref.var("armed").eq(True), True),
    (lambda m: m.ref.var("armed").ne(True), False),
    (lambda m: m.ref.var("armed").eq(False), False),
    (lambda m: m.ref.mem.i8[SRAM].masked(0xFF).eq(255), True),
    (lambda m: m.ref.mem.i8[SRAM].masked(0x0F).eq(0x0F), True),
    (lambda m: m.ref.mem.i8[SRAM].lt(0), True),
    (lambda m: m.ref.mem.u8[SRAM].eq(255), True),
    (lambda m: m.ref.var("wide").high.masked(0x3FFF).eq(0x3FFB), True),
]


@pytest.mark.parametrize(("build", "expected"), BUILT)
def test_a_condition_on_a_bit_field_or_mask_agrees_with_the_parked_path(
    m33: popcorn.PartDescription,
    build: Callable[[popcorn.Machine], popcorn.Condition],
    expected: bool,
) -> None:
    m = views_machine(m33)
    m.mem.i8[SRAM] = -1
    assert holds_both_ways(m, build(m)) is expected


def test_f32_condition_compares_in_double(m33: popcorn.PartDescription) -> None:
    m = views_machine(m33)
    m.mem.f32[SRAM] = 0.1
    assert not holds_both_ways(m, m.ref.mem.f32[SRAM].eq(0.1))
    assert holds_both_ways(m, m.ref.mem.f32[SRAM].gt(0.1))
    m.mem.f32[SRAM] = float("nan")
    assert holds_both_ways(m, m.ref.mem.f32[SRAM].ne(0.0))
    assert not holds_both_ways(m, m.ref.mem.f32[SRAM].eq(float("nan")))


def test_a_float_condition_takes_only_int_constants_a_double_holds(
    m33: popcorn.PartDescription,
) -> None:
    m = views_machine(m33)
    m.mem.f64[SRAM] = float(2**53)
    f64 = m.ref.mem.f64[SRAM]
    assert holds_both_ways(m, f64.eq(2**53))
    assert holds_both_ways(m, f64.lt(2**53 + 2))
    assert not holds_both_ways(m, f64.gt(2**53))
    for value in (2**53 + 1, -(2**53) - 1, 10**400):
        with pytest.raises(ValueError, match="not exactly a double"):
            f64.lt(value)


def test_a_condition_from_another_machine_is_refused(m33: popcorn.PartDescription) -> None:
    m, other = exact(m33), exact(m33)
    condition = other.ref.var("ticks").ge(1)
    with pytest.raises(ValueError, match="another machine"):
        asyncio.run(m.wait_for(condition, for_=ms(1)))
    with pytest.raises(ValueError, match="another machine"):
        waited(m, condition, ms(1))


def test_any_of_fires_where_a_parked_predicate_does(m33: popcorn.PartDescription) -> None:
    parked, running = exact(m33), exact(m33)

    def either(m: popcorn.Machine) -> bool:
        return int(m.var("ticks").value) >= 100 or int(m.var("loops").value) >= 5_000

    Gate(parked).opened.set()
    want = asyncio.run(parked.run(for_=ms(200), stop_when=either))
    ticks, loops = running.ref.var("ticks"), running.ref.var("loops")
    conditions = popcorn.any_of(ticks.ge(100), loops.ge(5_000))
    got = gated(running, conditions, ms(200))
    assert want.reason is popcorn.StopReason.USER
    assert (got.reason, got.time, got.pc) == (want.reason, want.time, want.pc)
    expected = tuple(
        member
        for member, held in zip(
            conditions.members,
            (int(parked.var("ticks").value) >= 100, int(parked.var("loops").value) >= 5_000),
            strict=True,
        )
        if held
    )
    assert expected
    assert got.fired == expected


def test_any_of_reports_both_members_true_on_one_event(m33: popcorn.PartDescription) -> None:
    m = exact(m33)
    ticks = m.ref.var("ticks")
    first, second = ticks.ge(100), ticks.gt(99)
    stop = waited(m, popcorn.any_of(ticks.eq(1_000_000), first, second), ms(200))
    assert stop.reason is popcorn.StopReason.USER
    assert stop.fired == (first, second)


def test_all_of_fires_when_every_member_holds(m33: popcorn.PartDescription) -> None:
    parked, running = exact(m33), exact(m33)

    def both(m: popcorn.Machine) -> bool:
        return int(m.var("ticks").value) >= 50 and int(m.var("loops").value) >= 2_000

    Gate(parked).opened.set()
    want = asyncio.run(parked.run(for_=ms(200), stop_when=both))
    conditions = popcorn.all_of(running.ref.var("ticks").ge(50), running.ref.var("loops").ge(2_000))
    got = gated(running, conditions, ms(200))
    assert want.reason is popcorn.StopReason.USER
    assert (got.reason, got.time, got.pc) == (want.reason, want.time, want.pc)
    assert got.fired == conditions.members


def test_conditions_time_out_with_nothing_fired(m33: popcorn.PartDescription) -> None:
    m = exact(m33)
    ticks = m.ref.var("ticks")
    conditions = popcorn.all_of(ticks.ge(10), ticks.eq(1_000_000))
    stop = waited(m, conditions, ms(20))
    assert stop.reason is popcorn.StopReason.TIME
    assert stop.fired == ()
    stop = asyncio.run(m.wait_for(conditions, for_=ms(20)))
    assert stop.reason is popcorn.StopReason.TIME
    assert stop.fired == ()


def test_conditions_parked_match_running(m33: popcorn.PartDescription) -> None:
    parked, running = exact(m33), exact(m33)
    sets = [
        popcorn.any_of(m.ref.var("ticks").ge(40), m.ref.var("ticks").ge(30))
        for m in (parked, running)
    ]
    Gate(parked).opened.set()
    want = asyncio.run(parked.wait_for(sets[0], for_=ms(200)))
    got = gated(running, sets[1], ms(200))
    assert (got.reason, got.time, got.pc) == (want.reason, want.time, want.pc)
    assert want.fired == (sets[0].members[1],)
    assert got.fired == (sets[1].members[1],)


def test_one_set_awaited_twice_reports_each_wait(m33: popcorn.PartDescription) -> None:
    m = exact(m33)
    ticks = m.ref.var("ticks")
    conditions = popcorn.any_of(ticks.ge(30), ticks.eq(1_000_000))
    gate = Gate(m)

    async def scenario() -> tuple[popcorn.Stop, popcorn.Stop]:
        async with m.running():
            assert await asyncio.to_thread(gate.entered.wait, HANG), "no write to ticks"
            waits = asyncio.gather(
                m.wait_for(conditions, for_=ms(200)), m.wait_for(conditions, for_=us(100))
            )
            await asyncio.sleep(0)
            gate.opened.set()
            return await waits

    fired, timed = run_bounded(scenario(), HANG)
    assert fired.reason is popcorn.StopReason.USER
    assert fired.fired == (conditions.members[0],)
    assert timed.reason is popcorn.StopReason.TIME
    assert timed.fired == ()


def test_conditions_refuse_nesting_empty_and_mixed_machines(
    m33: popcorn.PartDescription,
) -> None:
    m, other = exact(m33), exact(m33)
    condition = m.ref.var("ticks").ge(1)
    with pytest.raises(TypeError, match="Condition members only"):
        popcorn.any_of(popcorn.any_of(condition))  # pyright: ignore[reportArgumentType]
    with pytest.raises(ValueError, match="1 to 64"):
        popcorn.any_of()
    with pytest.raises(ValueError, match="1 to 64"):
        popcorn.all_of(*[condition] * 65)
    with pytest.raises(ValueError, match="one machine"):
        popcorn.any_of(condition, other.ref.var("ticks").ge(1))
    with pytest.raises(ValueError, match="another machine"):
        asyncio.run(other.wait_for(popcorn.all_of(condition), for_=ms(1)))
    with pytest.raises(AttributeError, match="immutable"):
        popcorn.any_of(condition).mode = "all"  # pyright: ignore[reportAttributeAccessIssue]
    with pytest.raises(AttributeError, match="immutable"):
        del popcorn.any_of(condition).members


def test_halt_from_another_machines_handler_is_refused_and_its_goal_goes_on(
    machines: Machines,
) -> None:
    a = machines()
    b = machines()
    refused: list[BaseException] = []

    def halting(call: popcorn.Call) -> popcorn.Return:
        try:
            a.halt()
        except RuntimeError as error:
            refused.append(error)
        return call.proceed()

    b.intercept("alu_mark", halting, once=True)

    async def scenario() -> popcorn.Stop:
        running = asyncio.create_task(a.run(budget=2_000_000))
        await asyncio.sleep(0)
        assert a.busy
        await b.run(budget=200_000)
        return await running

    stop = asyncio.run(scenario())
    assert stop.reason is popcorn.StopReason.BUDGET
    assert len(refused) == 1


def test_a_handler_stop_while_a_condition_holds_fires_nothing_parked(
    m33: popcorn.PartDescription,
) -> None:
    """The handler makes the condition hold and stops in one callback, with no event between in
    which the condition could fire."""
    m = exact(m33)
    flag = 0x2000_8000
    m.mem.u32[flag] = 0

    def set_then_stop(call: popcorn.Call) -> popcorn.Return:
        call.machine.mem.u32[flag] = 7
        return call.stop()

    m.intercept("SysTick_Handler", set_then_stop, once=True)
    stopped = asyncio.run(m.wait_for(m.ref.mem.u32[flag].eq(7), for_=ms(10)))
    assert stopped.reason is popcorn.StopReason.USER
    assert m.mem.u32[flag] == 7
    assert stopped.fired == ()


def test_elapse_passes_the_duration_in_a_free_run(machines: Machines) -> None:
    m = machines(slice_instructions=1_000)

    async def scenario() -> tuple[VirtualTime, VirtualTime, VirtualTime]:
        async with m.running():
            before = await m.now()
            first = await m.elapse(ms(1))
            second = await m.elapse(ms(1))
        return before, first, second

    before, first, second = run_bounded(scenario(), HANG)
    assert first >= before + ms(1)
    assert second >= first + ms(1)
    assert not m.busy


def test_elapse_in_a_free_run_raises_a_fault_once(machines: Machines) -> None:
    m = machines(slice_instructions=1_000)
    start = m.time

    async def scenario() -> None:
        moving = m.write((m.ref.reg.pc, 0x3000_0000), at=start + ms(50))
        async with m.running():
            elapsing = asyncio.create_task(m.elapse(ms(100)))
            await moving
            await elapsing

    with pytest.raises(popcorn.FaultError) as caught:
        run_bounded(scenario(), HANG)
    assert "letting time pass" in str(caught.value)
    assert caught.value.__notes__ == [f"background run stopped: {m.last_stop}"]
    assert caught.value.stop is not None
    assert start + ms(50) <= caught.value.stop.time < start + ms(100)
    assert not m.busy


def test_an_elapse_fault_caught_in_the_body_is_raised_again_on_leaving(
    machines: Machines,
) -> None:
    m = machines(slice_instructions=1_000)
    start = m.time
    caught: list[popcorn.FaultError] = []

    async def scenario() -> None:
        moving = m.write((m.ref.reg.pc, 0x3000_0000), at=start + ms(50))
        async with m.running():
            elapsing = asyncio.create_task(m.elapse(ms(100)))
            await moving
            try:
                await elapsing
            except popcorn.FaultError as error:
                caught.append(error)

    with pytest.raises(popcorn.FaultError) as left:
        run_bounded(scenario(), HANG)
    assert len(caught) == 1
    assert left.value is not caught[0]
    assert left.value.stop == caught[0].stop
    assert not m.busy


def test_cancelling_an_elapse_in_a_free_run_keeps_the_machine_running(
    machines: Machines,
) -> None:
    m = machines(slice_instructions=1_000)

    async def scenario() -> tuple[VirtualTime, VirtualTime]:
        async with m.running():
            elapsing = asyncio.create_task(m.elapse(s(1_000)))
            await asyncio.sleep(0)
            elapsing.cancel()
            with pytest.raises(asyncio.CancelledError):
                await elapsing
            assert m.busy
            before = await m.now()
            await m.elapse(ms(1))
            return before, await m.now()

    before, after = run_bounded(scenario(), HANG)
    assert after >= before + ms(1)


def test_now_in_a_free_run_is_monotonic(machines: Machines) -> None:
    m = machines(slice_instructions=1_000)

    async def scenario() -> list[VirtualTime]:
        async with m.running():
            times: list[VirtualTime] = []
            for _ in range(20):
                times.append(await m.now())
                await m.elapse(us(100))
            return times

    times = run_bounded(scenario(), HANG)
    assert times == sorted(times)
    assert times[-1] > times[0]


def test_wait_until_in_a_free_run_returns_the_boundary_where_it_held(
    m33: popcorn.PartDescription,
) -> None:
    m = ticking(m33)
    seen: list[tuple[VirtualTime, int]] = []

    def five_ticks(machine: popcorn.Machine) -> bool:
        ticks = int(machine.var("ticks").value)
        if ticks >= 5:
            seen.append((machine.time, ticks))
        return ticks >= 5

    async def scenario() -> tuple[VirtualTime | None, VirtualTime]:
        async with m.running():
            held = await m.wait_until(five_ticks, for_=s(10))
            return held, await m.now()

    held, after = run_bounded(scenario(), HANG)
    assert held is not None
    assert held <= after
    assert seen[0][0] == held
    assert seen[0][1] >= 5


def test_wait_until_in_a_free_run_returns_none_at_the_deadline(machines: Machines) -> None:
    m = machines(slice_instructions=1_000)

    async def scenario() -> tuple[VirtualTime, VirtualTime | None]:
        async with m.running():
            before = await m.now()
            return before, await m.wait_until(lambda _: False, for_=ms(1))

    before, held = run_bounded(scenario(), HANG)
    assert held is None
    assert not m.busy
    assert before < m.time


def test_wait_until_in_a_free_run_holding_at_its_deadline_returns_the_time(
    machines: Machines,
) -> None:
    m = machines(slice_instructions=10**9, slice_wall_budget=10.0)

    async def scenario() -> tuple[VirtualTime, VirtualTime | None]:
        async with m.running():
            start = await m.now()
            return start, await m.wait_until(lambda _: True, for_=ms(1))

    start, held = run_bounded(scenario(), HANG)
    assert held is not None
    assert held >= start + ms(1)


def test_cancelling_a_wait_until_in_a_free_run_keeps_the_machine_running(
    machines: Machines,
) -> None:
    m = machines(slice_instructions=1_000)

    async def scenario() -> tuple[VirtualTime, VirtualTime]:
        async with m.running():
            waiting = asyncio.create_task(m.wait_until(lambda _: False))
            await asyncio.sleep(0)
            waiting.cancel()
            with pytest.raises(asyncio.CancelledError):
                await waiting
            assert m.busy
            before = await m.now()
            await m.elapse(ms(1))
            return before, await m.now()

    before, after = run_bounded(scenario(), HANG)
    assert after >= before + ms(1)


def test_wait_until_in_a_free_run_gives_the_machine(machines: Machines) -> None:
    m = machines(slice_instructions=1_000)

    async def scenario() -> VirtualTime | None:
        async with m.running():
            return await m.wait_until(lambda machine: machine is m, for_=ms(1))

    assert run_bounded(scenario(), HANG) is not None


def failing_after_100(machine: popcorn.Machine) -> bool:
    """A predicate that fails once some instructions ran."""
    if machine.instructions > 100:
        raise LookupError("predicate failed")
    return False


def test_a_wait_until_error_reaches_the_caller_in_a_free_run(machines: Machines) -> None:
    m = machines(slice_instructions=1_000)

    async def scenario() -> None:
        async with m.running():
            await m.wait_until(failing_after_100, for_=ms(100))

    with pytest.raises(LookupError, match="predicate failed") as caught:
        run_bounded(scenario(), HANG)
    frames = [frame.name for frame in traceback.extract_tb(caught.value.__traceback__)]
    assert "failing_after_100" in frames
    assert not m.busy


def test_sample_reads_two_values_at_one_boundary(machines: Machines) -> None:
    m = machines(slice_instructions=1_000)

    async def scenario() -> list[tuple[int, int, int]]:
        async with m.running():
            sampled: list[tuple[int, int, int]] = []
            for _ in range(20):
                sampled.append(
                    await m.sample(
                        lambda: (m.instructions, int(m.var("alu_value").value), m.instructions)
                    )
                )
                await m.elapse(us(100))
            return sampled

    sampled = run_bounded(scenario(), HANG)
    assert all(first == last for first, _, last in sampled)
    assert sampled[-1][0] > sampled[0][0]


@pytest.mark.parametrize("name", REQUESTS_INSIDE)
def test_a_request_inside_a_running_sample_raises(machines: Machines, name: str) -> None:
    m = machines(slice_instructions=1_000)
    request, error = requests_inside(m)[name]

    async def scenario() -> None:
        async with m.running():
            with pytest.raises(error, match=REFUSED_INSIDE[error]) as raised:
                await m.sample(request)
            assert type(raised.value) is error
            await m.elapse(ms(1))

    run_bounded(scenario(), HANG)
    assert not m.busy


def test_a_stamped_running_sample_runs_at_or_after_its_time(machines: Machines) -> None:
    m = machines(slice_instructions=1_000)

    entered: list[VirtualTime] = []
    release = threading.Event()

    def hold() -> None:
        entered.append(m.time)
        release.wait(HANG)

    async def scenario() -> tuple[VirtualTime, VirtualTime]:
        async with m.running():
            held = m.sample(hold)
            while not entered:
                await asyncio.sleep(0.001)
            start = entered[0]
            stamped = m.sample(lambda: m.time, at=start + ms(1))
            release.set()
            await held
            at = await stamped
            with pytest.raises(ValueError):
                await m.sample(lambda: m.time, at=VirtualTime(0))
        return start, at

    start, at = run_bounded(scenario(), HANG)
    assert at >= start + ms(1)
    assert not m.busy


def sampled_past_the_halt(machines: Machines) -> weakref.ref[popcorn.Machine]:
    """A machine whose free run ended with a sample stamped past its halt still queued."""
    m = machines(slice_instructions=1_000)

    async def scenario() -> None:
        async with m.running():
            start = await m.now()
            m.sample(lambda: m.time, at=start + s(10))
            await m.elapse(ms(1))

    run_bounded(scenario(), HANG)
    return weakref.ref(m)


def test_a_sample_stamped_past_a_free_run_keeps_no_machine_alive(machines: Machines) -> None:
    gone = sampled_past_the_halt(machines)
    gc.collect()
    assert gone() is None


def test_a_running_sample_takes_the_gil_once_and_keeps_no_handler(machines: Machines) -> None:
    m = machines(slice_instructions=1_000)
    handle = m._handle  # pyright: ignore[reportPrivateUsage]
    table = m._callback_table  # pyright: ignore[reportPrivateUsage]
    handlers = len(table)

    async def scenario() -> int:
        async with m.running():
            await m.elapse(ms(1))
            before = handle.gil_entries()
            for _ in range(20):
                await m.sample(lambda: m.time)
            return handle.gil_entries() - before

    assert run_bounded(scenario(), HANG) == 20
    assert len(table) == handlers


def test_a_running_sample_error_reaches_the_await_and_the_machine_runs_on(
    machines: Machines,
) -> None:
    m = machines(slice_instructions=1_000)

    def failing() -> None:
        raise KeyError("sampled")

    async def scenario() -> tuple[VirtualTime, VirtualTime]:
        async with m.running():
            before = await m.now()
            with pytest.raises(KeyError, match="sampled"):
                await m.sample(failing)
            await m.elapse(ms(1))
            return before, await m.now()

    before, after = run_bounded(scenario(), HANG)
    assert after >= before + ms(1)
    assert not m.busy


RUNNING_SAMPLED: contextvars.ContextVar[str] = contextvars.ContextVar(
    "RUNNING_SAMPLED", default="unset"
)


def test_a_running_sample_runs_in_the_context_of_its_call(machines: Machines) -> None:
    m = machines(slice_instructions=1_000)

    async def scenario() -> str:
        async with m.running():
            RUNNING_SAMPLED.set("caller")
            return await m.sample(RUNNING_SAMPLED.get)

    assert run_bounded(scenario(), HANG) == "caller"


def test_a_slow_sample_stalls_the_machine(machines: Machines) -> None:
    m = machines(slice_instructions=1_000)

    def slow() -> tuple[int, int]:
        before = m.instructions
        time.sleep(0.05)
        return before, m.instructions

    async def scenario() -> tuple[int, int]:
        async with m.running():
            return await m.sample(slow)

    before, after = run_bounded(scenario(), HANG)
    assert before == after


def test_record_returns_slices_by_mark_while_running(m33: popcorn.PartDescription) -> None:
    m = ticking(m33)
    recording = m.record_returns("tick", lambda r: int(r.value or 0))

    def marked() -> tuple[int, VirtualTime]:
        return recording.mark(), m.time

    async def scenario() -> tuple[int, VirtualTime, int, VirtualTime]:
        async with m.running():
            await m.elapse(ms(1))
            mark, begun = await m.sample(marked)
            await m.elapse(ms(5))
            end, ended = await m.sample(marked)
        return mark, begun, end, ended

    mark, begun, end, ended = run_bounded(scenario(), HANG)
    entries = recording.since(mark)[: end - mark]
    span = ended - begun
    assert span >= ms(5)
    assert abs(len(entries) - span.ps / us(250).ps) <= 1
    times = [e.at for e in entries]
    assert times == sorted(set(times))
    assert begun <= times[0] and times[-1] <= ended
    first = entries[0].value
    assert [e.value for e in entries] == list(range(first, first + len(entries)))


def copied_until(
    mark: Callable[[], int], values_since: Callable[[int], list[int]], returns: int
) -> int:
    """Copy the newest entries of a recording in a loop until it recorded `returns`, each copy
    checked consecutive from its mark, and return the copies made."""
    copies = 0
    end = time.monotonic() + HANG
    while (now := mark()) < returns and time.monotonic() < end:
        start = max(0, now - 100)
        values = values_since(start)
        assert values == list(range(start + 1, start + 1 + len(values)))
        copies += 1
    return copies


@pytest.mark.parametrize(
    ("native", "returns"), [(True, 20_000), (False, 2_000)], ids=["native", "convert"]
)
def test_a_recording_read_in_a_loop_while_running_stays_consecutive(
    m33: popcorn.PartDescription, native: bool, returns: int
) -> None:
    m = ticking(m33)
    if native:
        raw = m.record_returns("tick")
        mark = raw.mark

        def values_since(start: int) -> list[int]:
            return [int(e.value.value or 0) for e in raw.since(start)]

    else:
        converted = m.record_returns("tick", lambda r: int(r.value or 0))
        mark = converted.mark

        def values_since(start: int) -> list[int]:
            return [e.value for e in converted.since(start)]

    interval = sys.getswitchinterval()

    async def scenario() -> int:
        async with m.running():
            return await asyncio.to_thread(copied_until, mark, values_since, returns)

    sys.setswitchinterval(1e-6)
    try:
        copies = run_bounded(scenario(), 2 * HANG)
    finally:
        sys.setswitchinterval(interval)
    assert copies > 10
    assert mark() >= returns


def test_a_recording_removed_while_running_keeps_its_entries(
    m33: popcorn.PartDescription,
) -> None:
    m = ticking(m33)
    recording = m.record_returns("tick")

    async def scenario() -> tuple[int, int]:
        async with m.running():
            await m.elapse(ms(2))
            recording.remove()
            removed = recording.mark()
            await m.elapse(ms(2))
            return removed, recording.mark()

    removed, later = run_bounded(scenario(), HANG)
    assert removed > 0
    assert later == removed
    assert len(recording.since()) == removed
    assert m.remove_hooks() == 0


@pytest.mark.parametrize("native", [True, False], ids=["native", "convert"])
def test_a_recording_without_convert_takes_no_gil_on_the_machine_thread(
    m33: popcorn.PartDescription, native: bool
) -> None:
    m = ticking(m33)
    recording = m.record_returns("tick") if native else m.record_returns("tick", tick_value)

    async def scenario() -> None:
        async with m.running():
            await m.elapse(ms(10))

    before = m._handle.gil_entries()  # pyright: ignore[reportPrivateUsage]
    run_bounded(scenario(), HANG)
    taken = m._handle.gil_entries() - before  # pyright: ignore[reportPrivateUsage]
    assert recording.mark() >= 20
    assert (taken == 0) is native


def tick_value(returned: popcorn.Returned) -> int:
    """The `ticks` count `tick` returned."""
    return int(returned.value or 0)


def test_a_recording_removed_by_a_handler_while_running_stops(
    m33: popcorn.PartDescription,
) -> None:
    m = ticking(m33)
    recording = m.record_returns("tick")

    def second(returned: popcorn.Returned) -> None:
        if returned.occurrence == 1:
            recording.remove()

    m.on_return("tick", second)

    async def scenario() -> tuple[int, int]:
        async with m.running():
            await m.elapse(ms(2))
            stopped = recording.mark()
            await m.elapse(ms(2))
            return stopped, recording.mark()

    stopped, later = run_bounded(scenario(), HANG)
    assert stopped in (1, 2)
    assert later == stopped
    assert m.remove_hooks() == 1


def test_a_deadline_wait_ends_at_its_absolute_time_while_running(machines: Machines) -> None:
    m = machines(slice_instructions=1_000)

    async def scenario() -> tuple[VirtualTime, popcorn.Stop, VirtualTime, popcorn.Stop]:
        async with m.running():
            start = await m.now()
            await m.elapse(ms(1))
            by_deadline = await m.wait_for(Never(), deadline=start + ms(3))
            later = await m.now()
            by_duration = await m.wait_for(Never(), for_=ms(3))
        return start, by_deadline, later, by_duration

    start, by_deadline, later, by_duration = run_bounded(scenario(), HANG)
    assert by_deadline.reason is popcorn.StopReason.TIME
    assert by_deadline.time >= start + ms(3)
    assert by_duration.time >= later + ms(3)


def test_a_parked_deadline_wait_ends_at_its_absolute_time(machines: Machines) -> None:
    m = machines(slice_instructions=1_000)
    start = m.time
    asyncio.run(m.run(for_=ms(1)))
    stop = asyncio.run(m.wait_for(Never(), deadline=start + ms(3)))
    assert stop.reason is popcorn.StopReason.TIME
    assert start + ms(3) <= stop.time < start + ms(3) + us(200)


@pytest.mark.parametrize("running", [False, True], ids=["parked", "running"])
def test_a_past_deadline_wait_returns_at_once(machines: Machines, running: bool) -> None:
    m = machines(slice_instructions=1_000)
    asyncio.run(m.run(for_=ms(1)))
    before = m.instructions

    async def waits() -> tuple[popcorn.Stop, VirtualTime | None]:
        past = VirtualTime(0)
        return (
            await m.wait_for(Never(), deadline=past),
            await m.wait_until(lambda _: False, deadline=past),
        )

    async def scenario() -> tuple[popcorn.Stop, VirtualTime | None]:
        if not running:
            return await waits()
        async with m.running():
            return await waits()

    stop, held = run_bounded(scenario(), HANG)
    assert stop.reason is popcorn.StopReason.TIME
    assert held is None
    if not running:
        assert m.instructions == before
        assert stop.instructions == before


def test_a_parked_deadline_wait_equals_the_same_wait_with_for(machines: Machines) -> None:
    m = machines()
    start = m.snapshot()
    by_deadline = asyncio.run(m.wait_for(AfterMarks(3), deadline=m.time + ms(5)))
    m.restore(start)
    by_duration = asyncio.run(m.wait_for(AfterMarks(3), for_=ms(5)))
    assert by_deadline == by_duration
    m.restore(start)
    assert asyncio.run(m.wait_until(lambda _: False, deadline=m.time + ms(1))) is None


@pytest.mark.parametrize("name", ["wait_for", "wait_until"])
def test_a_wait_refuses_both_for_and_deadline(machines: Machines, name: str) -> None:
    m = machines()
    before = m.instructions
    with pytest.raises(TypeError, match="exclusive"):
        if name == "wait_for":
            asyncio.run(m.wait_for(Never(), for_=ms(1), deadline=m.time))
        else:
            asyncio.run(m.wait_until(lambda _: False, for_=ms(1), deadline=m.time))
    assert m.instructions == before


def test_a_free_run_deadline_wait_ends_at_its_absolute_time(machines: Machines) -> None:
    m = machines(slice_instructions=1_000)

    async def scenario() -> (
        tuple[VirtualTime, VirtualTime, popcorn.Stop, VirtualTime, popcorn.Stop]
    ):
        async with m.running():
            start = await m.sample(lambda: m.time)
            await m.elapse(ms(20))
            first, ahead = await stalled_until_posted(
                m, lambda: m.wait_for(Never(), deadline=start + ms(25))
            )
            await m.elapse(ms(2))
            held, past = await stalled_until_posted(
                m, lambda: m.wait_for(Never(), deadline=start + ms(1))
            )
        return start, first, ahead, held, past

    start, first, ahead, held, past = run_bounded(scenario(), 6 * HANG)
    assert first >= start + ms(20)
    assert max(first, start + ms(25)) <= ahead.time < first + ms(20)
    assert held >= start + ms(25)
    assert past.reason is popcorn.StopReason.TIME
    assert held <= past.time < held + us(500)


def test_a_deadline_must_be_a_virtual_time(machines: Machines) -> None:
    m = machines()
    with pytest.raises(TypeError, match="VirtualTime"):
        asyncio.run(m.wait_for(Never(), deadline=cast(VirtualTime, ms(1))))
    with pytest.raises(TypeError, match="VirtualTime"):
        asyncio.run(m.wait_until(lambda _: False, deadline=cast(VirtualTime, ms(1))))


@pytest.mark.parametrize("running", [False, True], ids=["parked", "running"])
def test_a_negative_deadline_is_past(machines: Machines, running: bool) -> None:
    m = machines(slice_instructions=1_000)
    asyncio.run(m.run(for_=ms(1)))

    async def wait() -> popcorn.Stop:
        return await m.wait_for(Never(), deadline=VirtualTime(-5))

    async def scenario() -> popcorn.Stop:
        if not running:
            return await wait()
        async with m.running():
            return await wait()

    assert run_bounded(scenario(), HANG).reason is popcorn.StopReason.TIME


def test_a_deadline_past_64_bits_never_comes(machines: Machines) -> None:
    m = machines()
    stop = asyncio.run(m.wait_for(AfterInstructions(5_000), deadline=VirtualTime(2**70)))
    assert stop.reason is popcorn.StopReason.USER
