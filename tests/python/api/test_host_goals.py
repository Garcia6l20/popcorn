"""The goals of the machine host: the machine runs while a sync body blocks, awaiting firmware
waits on the machine's thread without turning the event loop, and the host's edges around it."""

from __future__ import annotations

import asyncio
import gc
import selectors
import socket
import subprocess
import sys
import textwrap
import threading
import time
from collections.abc import Callable, Coroutine

import popcorn
import pytest
from popcorn import _core
from popcorn.units import VirtualTime, us

from .conftest import DRIVER_ELF, M33_JSON, Machines, run_bounded

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
    began = time.monotonic()
    del m
    gc.collect()
    assert time.monotonic() - began < 1
    assert _core.host_threads() == before


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
