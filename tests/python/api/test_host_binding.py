"""The native host under the binding: callbacks on the machine's thread, in captured contexts,
the future bridge, destruction, shutdown and the lock order."""

from __future__ import annotations

import asyncio
import contextlib
import contextvars
import functools
import gc
import logging
import pathlib
import subprocess
import sys
import textwrap
import threading
import time
import weakref
from collections.abc import Callable
from typing import Any

import popcorn
import pytest
from popcorn import _core
from popcorn._host import awaited
from popcorn._refs import Pending
from popcorn._stop import Stop, stop_of
from popcorn.model import Model, on_read
from popcorn.units import VirtualTime, s, us

from .conftest import BOARD_ELF, DRIVER_ELF, M33_JSON, Machines
from .devices_example import Eeprom24
from .projects import MODEL_ELF, MODEL_JSON

VALUE: contextvars.ContextVar[str] = contextvars.ContextVar("host_binding_value", default="unset")
HOLDER: contextvars.ContextVar[list[object]] = contextvars.ContextVar("host_binding_holder")

CHILD_TIMEOUT = 60
COLLECTED = "a model or device of a collected Machine was called"


def stop_info(raw: object) -> Stop:
    assert isinstance(raw, _core.StopInfo)
    return stop_of(raw)


def settles(reached: Callable[[], bool], seconds: float = 5) -> bool:
    until = time.monotonic() + seconds
    while not reached():
        if time.monotonic() > until:
            return False
        time.sleep(0.001)
    return True


class Recorder(Model):
    """Serves the model firmware's TIMER, recording each CNT read and reading memory there."""

    def __init__(self) -> None:
        super().__init__()
        self.values: list[str] = []
        self.threads: list[int] = []
        self.words: list[int] = []
        self.machine: popcorn.Machine | None = None

    @on_read("CNT")
    def _cnt(self) -> int:
        self.values.append(VALUE.get())
        self.threads.append(threading.get_ident())
        if self.machine is not None:
            self.words.append(self.machine.mem.u32[self.machine.sym("timer_irqs")])
        return 7


def recorder_machine() -> tuple[popcorn.Machine, Recorder]:
    m = popcorn.Machine(
        popcorn.load_description(MODEL_JSON),
        image=popcorn.Image.elf(MODEL_ELF),
        models={"TIMER": Recorder},
    )
    m.reset()
    stop = asyncio.run(m.run(until=m.sym("ready"), budget=10_000))
    assert stop.reason is popcorn.StopReason.ADDRESS
    model = m.model("TIMER")
    assert isinstance(model, Recorder)
    return m, model


def read_count(m: popcorn.Machine) -> int:
    result = asyncio.run(m.call("read_timer_count", budget=100_000))
    assert result.stop.reason is popcorn.StopReason.RETURNED
    return int(result.value)


def gil_entries(m: popcorn.Machine) -> int:
    return m._handle.gil_entries()  # pyright: ignore[reportPrivateUsage]


def rounds_at_least(m: popcorn.Machine, count: int) -> popcorn.Condition:
    return m.ref.mem.u32[m.sym("alu_rounds")].ge(count)


def test_a_free_run_without_python_callbacks_never_takes_the_gil(machines: Machines) -> None:
    m = machines(slice_instructions=1_000)
    m.start()
    stop = asyncio.run(m.wait_for(rounds_at_least(m, 1)))
    assert stop.reason is popcorn.StopReason.USER
    assert m.halt() is None
    assert m.instructions > 0
    assert gil_entries(m) == 0


def test_the_first_callback_creates_the_thread_state_in_its_own_gil_entry(
    machines: Machines,
) -> None:
    """The cycle collector is off meanwhile: run on the machine's thread, it would finalize
    earlier tests' garbage there, whose native destructors take the GIL too."""
    m = machines(slice_instructions=1_000)
    hits: list[int] = []
    m.watch("alu_value", handler=lambda hit: hits.append(hit.address))
    gc.collect()
    gc.disable()
    try:
        m.start()
        asyncio.run(m.wait_for(rounds_at_least(m, 1)))
        assert m.halt() is None
    finally:
        gc.enable()
    assert len(hits) > 1000
    assert gil_entries(m) == len(hits)


def timer_started_in_a_free_run() -> popcorn.Machine:
    """A machine of the model firmware with no Python model, parked to run `start_timer(10)`
    then return into the reset loop: its writes of TIMER.ARR and TIMER.CR, two unmodelled
    registers, log two records."""
    m = popcorn.Machine(popcorn.load_description(MODEL_JSON), image=popcorn.Image.elf(MODEL_ELF))
    m.reset()
    asyncio.run(m.run(until=m.sym("ready"), budget=10_000))
    m.reg.r0 = 10
    m.reg.lr = m.sym("ready") | 1
    m.reg.pc = m.sym("start_timer") & ~1
    return m


def logged_timer(caplog: pytest.LogCaptureFixture) -> list[logging.LogRecord]:
    return [r for r in caplog.records if getattr(r, "popcorn_peripheral", None) == "TIMER"]


def never(m: popcorn.Machine) -> popcorn.Condition:
    return m.ref.mem.u32[m.sym("sink")].eq(0xDEAD_BEEF)


def test_a_free_run_queues_its_log_records_for_the_next_await(
    caplog: pytest.LogCaptureFixture,
) -> None:
    m = timer_started_in_a_free_run()
    with caplog.at_level(logging.WARNING, logger="popcorn"):
        m.start()
        stop = asyncio.run(m.wait_for(never(m), for_=us(50)))
        assert stop.reason is popcorn.StopReason.TIME
        logged = logged_timer(caplog)
        assert m.halt() is None
    assert [r.popcorn_register for r in logged] == ["ARR", "CR"]  # type: ignore[attr-defined]
    start = m.symbols["start_timer"]
    for record in logged:
        fields = vars(record)
        assert start.value & ~1 <= fields["popcorn_pc"] < (start.value & ~1) + start.size
        assert isinstance(fields["popcorn_time"], VirtualTime)
        assert 0 < fields["popcorn_time"].ps <= stop.time.ps
        assert record.threadName == threading.current_thread().name
    assert gil_entries(m) == 0


def test_records_queued_before_the_first_await_reach_logging_during_it(
    caplog: pytest.LogCaptureFixture,
) -> None:
    """The records land while no loop waits, before the first await registers the loop's waker:
    that registration wakes the loop for them, so they arrive while the wait still runs."""
    m = timer_started_in_a_free_run()

    async def scenario() -> bool:
        m.start()
        time.sleep(0.05)
        waiting = asyncio.create_task(m.wait_for(never(m), for_=us(5_000_000)))
        while not logged_timer(caplog) and not waiting.done():
            await asyncio.sleep(0.005)
        arrived_first = not waiting.done()
        waiting.cancel()
        with contextlib.suppress(asyncio.CancelledError):
            await waiting
        return arrived_first

    with caplog.at_level(logging.WARNING, logger="popcorn"):
        assert asyncio.run(scenario())
    m.halt()


class FailOnce(logging.Filter):
    """Raises on the first record it sees, passes the others."""

    def __init__(self) -> None:
        super().__init__()
        self.failed = False

    def filter(self, record: logging.LogRecord) -> bool:
        if not self.failed:
            self.failed = True
            raise RuntimeError("filter failed")
        return True


def test_a_raising_log_filter_raises_from_the_next_halt_and_keeps_the_rest(
    caplog: pytest.LogCaptureFixture,
) -> None:
    m = timer_started_in_a_free_run()
    failing = FailOnce()
    logger = logging.getLogger("popcorn")
    logger.addFilter(failing)
    try:
        with caplog.at_level(logging.WARNING, logger="popcorn"):
            m.start()
            stop = asyncio.run(m.wait_for(never(m), for_=us(50)))
            assert stop.reason is popcorn.StopReason.TIME
            with pytest.raises(RuntimeError, match="filter failed"):
                m.halt()
            assert not m.busy
            m._flush_logs()  # pyright: ignore[reportPrivateUsage]
            logged = logged_timer(caplog)
    finally:
        logger.removeFilter(failing)
    assert [r.popcorn_register for r in logged] == ["CR"]  # type: ignore[attr-defined]
    assert m.instructions > 0


def queued_without_a_flush(m: popcorn.Machine) -> None:
    """Start `m` and return once its two TIMER records are queued, without forwarding them: a
    raw wait read through a done callback flushes nothing."""
    done = threading.Event()
    m.start()
    future = m._handle.wait(None, us(50).ps)  # pyright: ignore[reportPrivateUsage]
    future.add_done_callback(done.set)
    assert done.wait(10)


def test_a_loop_flush_keeps_a_filter_error_for_the_next_raising_flush(
    caplog: pytest.LogCaptureFixture,
) -> None:
    m = timer_started_in_a_free_run()
    failing = FailOnce()
    logger = logging.getLogger("popcorn")
    logger.addFilter(failing)
    try:
        with caplog.at_level(logging.WARNING, logger="popcorn"):
            queued_without_a_flush(m)
            m._flush_logs()  # pyright: ignore[reportPrivateUsage]
            assert failing.failed
            assert logged_timer(caplog) == []
            with pytest.raises(RuntimeError, match="filter failed"):
                m.halt()
            assert logged_timer(caplog) == []
            m._flush_logs()  # pyright: ignore[reportPrivateUsage]
            logged = logged_timer(caplog)
    finally:
        logger.removeFilter(failing)
    assert [r.popcorn_register for r in logged] == ["CR"]  # type: ignore[attr-defined]


def test_a_kept_log_error_is_reported_when_its_machine_is_collected(
    caplog: pytest.LogCaptureFixture, monkeypatch: pytest.MonkeyPatch
) -> None:
    reported: list[str] = []

    def report(raised: sys.UnraisableHookArgs) -> None:
        reported.append(repr(raised.exc_value))

    monkeypatch.setattr(sys, "unraisablehook", report)
    m = timer_started_in_a_free_run()
    failing = FailOnce()
    logger = logging.getLogger("popcorn")
    logger.addFilter(failing)
    try:
        with caplog.at_level(logging.WARNING, logger="popcorn"):
            queued_without_a_flush(m)
            m._flush_logs()  # pyright: ignore[reportPrivateUsage]
            assert failing.failed
            probe = weakref.ref(m)
            del m
            gc.collect()
            assert probe() is None
    finally:
        logger.removeFilter(failing)
    assert reported == ["RuntimeError('filter failed')"]


def gpio_holes(description: popcorn.MachineDescription, count: int) -> list[int]:
    """Byte addresses of GPIO windows that no register covers, each logged once when read."""
    holes: list[int] = []
    for port in "ABCDEFGHI":
        found = next(p for p in description.peripherals if p.name == f"GPIO{port}")
        taken = {r.offset + k for r in found.registers for k in range(4)}
        holes.extend(found.base + offset for offset in range(found.size) if offset not in taken)
    assert len(holes) >= count
    return holes[:count]


def test_a_parked_access_sends_a_log_error_to_the_unraisable_hook(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    from popcorn.stm32 import mcu

    reported: list[BaseException | None] = []

    def report(raised: sys.UnraisableHookArgs) -> None:
        reported.append(raised.exc_value)

    monkeypatch.setattr(sys, "unraisablehook", report)
    description = mcu("STM32U575QGIx")
    m = popcorn.Machine(description)
    (hole,) = gpio_holes(description, 1)
    failing = FailOnce()
    logger = logging.getLogger("popcorn")
    logger.addFilter(failing)
    try:
        assert m.mem.u8[hole] == 0
    finally:
        logger.removeFilter(failing)
    assert len(reported) == 1 and "filter failed" in str(reported[0])


def test_a_flush_forwards_only_the_records_queued_when_it_began(
    caplog: pytest.LogCaptureFixture,
) -> None:
    from popcorn.stm32 import mcu

    description = mcu("STM32U575QGIx")
    m = popcorn.Machine(description)
    holes = gpio_holes(description, 50)
    reads = iter(holes[1:])

    class Reading(logging.Handler):
        def emit(self, record: logging.LogRecord) -> None:
            following = next(reads, None)
            if following is not None:
                m.mem.u8[following]

    reading = Reading()
    logger = logging.getLogger("popcorn")
    logger.addHandler(reading)
    try:
        with caplog.at_level(logging.WARNING, logger="popcorn"):
            m._handle.parked(lambda: m.mem.u8[holes[0]])  # pyright: ignore[reportPrivateUsage]
            first = len(caplog.records)
            m._flush_logs()  # pyright: ignore[reportPrivateUsage]
            second = len(caplog.records)
    finally:
        logger.removeHandler(reading)
        m._flush_logs()  # pyright: ignore[reportPrivateUsage]
    assert (first, second) == (1, 2)


def test_past_4096_queued_log_records_the_rest_are_counted_in_one_warning(
    caplog: pytest.LogCaptureFixture,
) -> None:
    from popcorn.stm32 import mcu

    description = mcu("STM32U575QGIx")
    m = popcorn.Machine(description)
    holes: list[int] = []
    for port in "ABCDEFGHI":
        found = next(p for p in description.peripherals if p.name == f"GPIO{port}")
        taken = {r.offset + k for r in found.registers for k in range(4)}
        holes.extend(found.base + offset for offset in range(found.size) if offset not in taken)
    assert len(holes) >= 5000

    def touch() -> None:
        for address in holes[:5000]:
            m.mem.u8[address]

    with caplog.at_level(logging.WARNING, logger="popcorn"):
        m._handle.parked(touch)  # pyright: ignore[reportPrivateUsage]
    messages = [r.getMessage() for r in caplog.records if r.name == "popcorn"]
    assert len(messages) == 4097
    assert messages[0].endswith(f"at offset {holes[0] - 0x4202_0000:#x}, no register there")
    assert messages[-1].endswith("904 log records were dropped, the queue holds 4096")


def test_a_handler_runs_on_the_machine_thread(machines: Machines) -> None:
    m = machines()
    threads: list[int] = []
    m.intercept("alu_mark", lambda call: threads.append(threading.get_ident()) or call.proceed())
    asyncio.run(m.run(budget=1_000_000))
    assert len(threads) == 1
    assert threads[0] != threading.get_ident()


def test_a_handler_sees_the_context_of_its_install(machines: Machines) -> None:
    m = machines()
    seen: list[str] = []

    def install() -> None:
        VALUE.set("before")
        m.intercept("alu_mark", lambda call: seen.append(VALUE.get()) or call.proceed())
        VALUE.set("after")
        asyncio.run(m.run(budget=1_000_000))

    contextvars.copy_context().run(install)
    assert seen == ["before"]


def test_a_python_model_sees_the_context_of_its_machine() -> None:
    def build() -> tuple[popcorn.Machine, Recorder]:
        VALUE.set("at construction")
        made = recorder_machine()
        VALUE.set("later")
        return made

    m, model = contextvars.copy_context().run(build)
    assert read_count(m) == 7
    assert model.values == ["at construction"]


def test_a_stop_predicate_sees_the_context_of_its_run(machines: Machines) -> None:
    m = machines()
    seen: list[str] = []

    def predicate(_: popcorn.Machine) -> bool:
        seen.append(VALUE.get())
        return False

    async def body() -> popcorn.Stop:
        VALUE.set("in the task")
        return await m.run(budget=200_000, stop_when=predicate)

    assert asyncio.run(body()).reason is popcorn.StopReason.BUDGET
    assert seen and set(seen) == {"in the task"}


def test_a_python_model_reads_memory_on_the_machine_thread() -> None:
    m, model = recorder_machine()
    model.machine = m
    try:
        assert read_count(m) == 7
    finally:
        model.machine = None
    assert model.words == [m.mem.u32[m.sym("timer_irqs")]]
    assert model.threads[0] != threading.get_ident()


def test_a_goal_future_is_awaited_from_two_loops_then_waited_without_one(
    machines: Machines,
) -> None:
    m = machines()

    def goal() -> _core.HostFuture:
        handle = m._handle  # pyright: ignore[reportPrivateUsage]
        return handle.run_goal(until=None, budget=5_000, exact=None, for_ps=None, stop_when=None)

    first = asyncio.run(awaited(goal(), stop_info))
    second = asyncio.run(awaited(goal(), stop_info))
    assert first.reason is second.reason is popcorn.StopReason.BUDGET
    assert second.instructions > first.instructions
    pending = goal()
    results: list[object] = []
    waiter = threading.Thread(target=lambda: results.append(pending.result(None)))
    waiter.start()
    waiter.join(10)
    assert len(results) == 1
    assert stop_info(results[0]).instructions > second.instructions
    assert pending.done()


def test_cancelling_the_awaiting_task_halts_the_goal(machines: Machines) -> None:
    m = machines()

    async def body() -> None:
        endless = m._handle.run_goal(  # pyright: ignore[reportPrivateUsage]
            until=None, budget=10**15, exact=None, for_ps=None, stop_when=None
        )
        task = asyncio.create_task(awaited(endless, stop_info))
        await asyncio.sleep(0.02)
        task.cancel()
        with pytest.raises(asyncio.CancelledError):
            await task
        assert endless.done()
        with pytest.raises(popcorn.EmulationError, match="cancelled"):
            endless.result(5)

    asyncio.run(body())
    assert not m.busy
    assert asyncio.run(m.run(budget=1_000)).reason is popcorn.StopReason.BUDGET


def test_a_collected_machine_joins_its_thread(machines: Machines) -> None:
    gc.collect()
    before = _core.host_threads()
    m = machines()
    asyncio.run(m.step(1))
    assert _core.host_threads() == before + 1
    del m
    gc.collect()
    assert _core.host_threads() == before


def test_a_machine_never_run_starts_no_thread(machines: Machines) -> None:
    gc.collect()
    before = _core.host_threads()
    m = machines()
    assert m.mem.u32[m.sym("alu_rounds")] == 0
    assert _core.host_threads() == before


def run_child(script: str, timeout: float) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, "-c", textwrap.dedent(script)],
        capture_output=True,
        text=True,
        timeout=timeout,
        check=False,
    )


def test_an_interpreter_exits_with_a_live_machine_thread() -> None:
    child = run_child(
        f"""
        import asyncio
        import popcorn

        m = popcorn.Machine(
            popcorn.load_description({str(M33_JSON)!r}),
            image=popcorn.Image.elf({str(DRIVER_ELF)!r}),
        )
        m.reset()
        asyncio.run(m.run(budget=10_000))
        holder = [m]
        holder.append(holder)
        """,
        timeout=10,
    )
    assert child.returncode == 0, child.stderr


def test_a_collection_inside_a_handler_destroys_another_machine(machines: Machines) -> None:
    gc.collect()
    before = _core.host_threads()
    first = machines()
    second = machines()
    asyncio.run(second.step(1))
    assert _core.host_threads() == before + 1

    class Box:
        def __init__(self, machine: popcorn.Machine) -> None:
            self.machine = machine
            self.itself = self

    collected: list[int] = []

    def handler(call: popcorn.Call) -> popcorn.Return:
        HOLDER.get().clear()
        collected.append(gc.collect())
        return call.proceed()

    def install(target: popcorn.Machine) -> None:
        HOLDER.set([Box(target)])
        first.intercept("alu_mark", handler)

    contextvars.copy_context().run(install, second)
    del second
    asyncio.run(first.run(budget=1_000_000))
    assert len(collected) == 1 and collected[0] > 0
    assert settles(lambda: _core.host_threads() == before + 1)


def test_a_context_copy_runs_while_its_source_is_entered_on_the_loop(machines: Machines) -> None:
    m = machines()
    predicate_values: list[str] = []
    handler_values: list[str] = []

    def predicate(_: popcorn.Machine) -> bool:
        predicate_values.append(VALUE.get())
        return False

    async def body() -> popcorn.Stop:
        VALUE.set("task")
        m.intercept("alu_mark", lambda call: handler_values.append(VALUE.get()) or call.proceed())
        return await m.run(budget=1_000_000, stop_when=predicate)

    assert asyncio.run(body()).reason is popcorn.StopReason.BUDGET
    assert predicate_values and set(predicate_values) == {"task"}
    assert handler_values == ["task"]


STRESS = """
import asyncio
import contextlib
import sys
import threading
import time

import popcorn
from popcorn.model import Model, on_write
from popcorn.stm32 import mcu
from popcorn.units import ms, us

TIM6_CR1 = 0x4000_1000


class Probe(Model):
    writes = 0

    @on_write("CR1")
    def _cr1(self, old, new):
        self.writes += 1


class Never:
    def ready(self, machine):
        return False


sys.setswitchinterval(1e-5)
m = popcorn.Machine(
    mcu("STM32U575QGIx"), image=popcorn.Image.elf({board!r}), models={{"TIM6": Probe}}
)
m.reset()
asyncio.run(m.run(until=m.sym("ready"), budget=1_000_000))
hits = []
m.watch("pin_seen", handler=lambda hit: hits.append(1) and None)
uart = m.periph.USART1.port
audio = m.periph.SAI1.audio("B")
cr1 = m.ref.mem.u32[TIM6_CR1]
done = threading.Event()
counts = {{"a": 0, "posted": 0, "withdrawn": 0, "c": 0, "audio": 0, "sessions": 0}}


async def posts(n):
    (pc,) = await m.read(m.ref.pc)
    await m.write((cr1, n & 1))
    counts["posted"] += 1
    now = (await m.read()).time
    stamped = asyncio.ensure_future(m.write((cr1, 1), at=now + ms(1000)))
    await asyncio.sleep(0)
    stamped.cancel()
    with contextlib.suppress(asyncio.CancelledError):
        await stamped
    counts["withdrawn"] += 1


def writer():
    n = 0
    while not done.is_set():
        n += 1
        try:
            m.mem.u32[TIM6_CR1] = n & 1
            counts["a"] += 1
        except (popcorn.MachineRunning, popcorn.MachineBusy):
            pass
        asyncio.run(posts(n))


def drainer():
    while not done.is_set():
        if uart.available:
            uart._link.drain(uart.available)
            counts["c"] += 1
        asyncio.run(audio.write([1]))
        counts["audio"] += audio.available + 1


async def session():
    m.start()
    try:
        await m.wait_for(Never(), for_=us(200))
        assert m._handle.session_open()
        waiter = m._handle.wait(None, us(100).ps)
        waiter.result(5)
    finally:
        m.halt()


failures = []
threading.excepthook = lambda args: failures.append(repr(args.exc_value))
threads = [threading.Thread(target=writer), threading.Thread(target=drainer)]
for t in threads:
    t.start()
until = time.monotonic() + 2
runs = 0
while time.monotonic() < until:
    asyncio.run(uart.write(b"x"))
    asyncio.run(m.run(budget=10_000))
    asyncio.run(session())
    counts["sessions"] += 1
    runs += 1
done.set()
for t in threads:
    t.join()
assert not failures, failures
assert runs > 0 and counts["a"] > 0 and counts["c"] > 0 and hits, (runs, counts, len(hits))
assert counts["posted"] > 0 and counts["withdrawn"] > 0 and counts["audio"] > 0, counts
print(runs, counts, len(hits))
"""


CLOSED = """
import asyncio

import popcorn
from popcorn import _core
from popcorn.units import us

m = popcorn.Machine(popcorn.load_description({m33!r}), image=popcorn.Image.elf({driver!r}))
m.reset()
pending = m.write((m.ref.reg.r0, 1), at=m.time + us(5))
_core.shutdown_hosts()
try:
    asyncio.run(pending)
except popcorn.EmulationError as error:
    print(error)
"""


def test_a_command_pending_when_its_machine_closes_raises_closed() -> None:
    child = run_child(CLOSED.format(m33=str(M33_JSON), driver=str(DRIVER_ELF)), CHILD_TIMEOUT)
    assert child.returncode == 0, child.stderr
    assert child.stdout.strip() == "the machine was closed"


def test_the_lock_order_holds_under_contention(u575: popcorn.MachineDescription) -> None:
    from .conftest import BOARD_ELF

    del u575
    child = run_child(STRESS.format(board=str(BOARD_ELF)), timeout=CHILD_TIMEOUT)
    assert child.returncode == 0, child.stderr


CURRENT: contextvars.ContextVar[object] = contextvars.ContextVar("host_binding_current")


def test_a_context_holding_the_machine_does_not_keep_it_alive(
    m33: popcorn.MachineDescription, driver: popcorn.Image
) -> None:
    gc.collect()
    before = _core.host_threads()
    cell: list[object] = []

    def build() -> weakref.ref[popcorn.Machine]:
        CURRENT.set(cell)
        m = popcorn.Machine(m33, image=driver)
        m.reset()
        cell.append(m)
        CURRENT.set(m)

        def handler(call: popcorn.Call) -> popcorn.Return:
            CURRENT.set(call)
            return call.proceed()

        m.intercept("alu_mark", handler)
        assert asyncio.run(m.run(budget=1_000_000)).reason is popcorn.StopReason.BUDGET
        assert _core.host_threads() == before + 1
        return weakref.ref(m)

    probe = contextvars.copy_context().run(build)
    cell.clear()
    gc.collect()
    assert probe() is None
    assert _core.host_threads() == before


DROPPED_HOOKS = """
import asyncio
import gc
import time

import popcorn
from popcorn import _core

m = popcorn.Machine(
    popcorn.load_description({m33!r}), image=popcorn.Image.elf({driver!r})
)
m.reset()
handle = m._handle
target = m.sym("alu_rounds")
for _ in range(3):
    hooks = [
        handle.add_watch(target, 4, _core.WatchKind.write, lambda hit: _core.WatchAction.resume)
        for _ in range(64)
    ]
    trace = handle.open_trace({trace!r}, [0], [])
    goal = handle.run_goal(until=None, budget=10**15, exact=None, for_ps=None, stop_when=None)
    time.sleep(0.02)
    del hooks, trace
    gc.collect()
    time.sleep(0.02)
    assert not goal.done()
    goal.cancel()
    try:
        goal.result(5)
    except popcorn.EmulationError:
        pass
assert asyncio.run(m.run(budget=1_000)).reason is popcorn.StopReason.BUDGET
print("ok")
"""


def test_hooks_and_traces_dropped_during_a_run_are_removed_at_a_boundary(
    tmp_path: pathlib.Path,
) -> None:
    script = DROPPED_HOOKS.format(
        m33=str(M33_JSON), driver=str(DRIVER_ELF), trace=str(tmp_path / "dropped.pctr")
    )
    child = run_child(script, timeout=CHILD_TIMEOUT)
    assert child.returncode == 0, child.stderr
    assert child.stdout.strip() == "ok"


def test_an_interpreter_finalizing_with_a_live_machine_thread_exits() -> None:
    child = run_child(
        f"""
        import asyncio
        import atexit
        import popcorn
        from popcorn import _core

        atexit.unregister(_core.shutdown_hosts)
        m = popcorn.Machine(
            popcorn.load_description({str(M33_JSON)!r}),
            image=popcorn.Image.elf({str(DRIVER_ELF)!r}),
        )
        m.reset()
        asyncio.run(m.run(budget=10_000))
        assert _core.host_threads() == 1
        holder = [m]
        holder.append(holder)
        """,
        timeout=10,
    )
    assert child.returncode == 0, child.stderr


SELF_ENDED = """
import asyncio
import gc
import sys
import threading

import popcorn

KIND, SURFACED, NATIVE, CONDITION = __CASE__
if NATIVE:
    del popcorn.Machine.__del__
reported = []
sys.unraisablehook = lambda raised: reported.append(repr(raised.exc_value))
ended = threading.Event()


class Never:
    def ready(self, machine):
        return False


def handler(call):
    if KIND == "raise":
        raise ValueError("boom")
    return call.stop()


m = popcorn.Machine(popcorn.load_description(__JSON__), image=popcorn.Image.elf(__ELF__))
m.reset()
m.intercept("alu_mark", handler)
event = m.ref.mem.u32[0x2000_8000].eq(0xDEADBEEF) if CONDITION else Never()
m.start()
if SURFACED:
    try:
        stop = asyncio.run(m.wait_for(event))
        assert stop.reason is popcorn.StopReason.USER and stop.fired == (), stop
    except ValueError:
        assert KIND == "raise"
else:
    end = m._handle.wait(None, None)
    end.add_done_callback(ended.set)
    assert ended.wait(10)
    del end
del m
gc.collect()
if SURFACED:
    assert reported == [], reported
elif KIND == "stop":
    assert len(reported) == 1 and "never returned to a caller" in reported[0], reported
else:
    assert len(reported) == 1 and reported[0].startswith("EmulationError("), reported
    assert "ValueError: boom" in reported[0], reported
print("ok")
"""


@pytest.mark.parametrize("native", [False, True], ids=["del", "destructor"])
@pytest.mark.parametrize("surfaced", [False, True], ids=["unclaimed", "surfaced"])
@pytest.mark.parametrize("kind", ["stop", "raise"])
def test_a_collected_machine_reports_only_an_end_no_wait_delivered(
    kind: str, surfaced: bool, native: bool
) -> None:
    script = (
        SELF_ENDED.replace("__CASE__", repr((kind, surfaced, native, False)))
        .replace("__JSON__", repr(str(M33_JSON)))
        .replace("__ELF__", repr(str(DRIVER_ELF)))
    )
    child = run_child(script, timeout=CHILD_TIMEOUT)
    assert child.returncode == 0, child.stderr
    assert child.stdout.strip() == "ok"


@pytest.mark.parametrize("native", [False, True], ids=["del", "destructor"])
@pytest.mark.parametrize("kind", ["stop", "raise"])
def test_a_collected_machine_reports_no_end_a_condition_wait_delivered(
    kind: str, native: bool
) -> None:
    script = (
        SELF_ENDED.replace("__CASE__", repr((kind, True, native, True)))
        .replace("__JSON__", repr(str(M33_JSON)))
        .replace("__ELF__", repr(str(DRIVER_ELF)))
    )
    child = run_child(script, timeout=CHILD_TIMEOUT)
    assert child.returncode == 0, child.stderr
    assert child.stdout.strip() == "ok"


STALE_WAIT = """
import asyncio
import gc
import sys
import threading

sys.path.insert(0, __API__)

import popcorn
from models_example import Timer

KIND = __KIND__
reported = []
sys.unraisablehook = lambda raised: reported.append(repr(raised.exc_value))
count = [0]


def handler(call):
    count[0] += 1
    if KIND == "raise":
        raise ValueError(f"boom{count[0]}")
    return call.stop()


def ended(m):
    done = threading.Event()
    future = m._handle.wait(None, None)
    future.add_done_callback(done.set)
    assert done.wait(10)
    return future


m = popcorn.Machine(
    popcorn.load_description(__JSON__), image=popcorn.Image.elf(__ELF__), models=dict(TIMER=Timer)
)
m.reset()
asyncio.run(m.run(until=m.sym("ready"), budget=10_000))
asyncio.run(m.call("start_timer", 1, budget=10_000))
m.intercept("TIMER_IRQHandler", handler)
m.start()
stale = ended(m)
try:
    m.halt()
except Exception:
    pass
m.start()
current = ended(m)
if KIND == "stop":
    current.result(0.0)
del current
try:
    stale.result(0.0)
except ValueError:
    pass
del stale, m
gc.collect()
expected = 1 if KIND == "raise" else 0
assert count[0] == 2, count
assert len(reported) == expected, reported
if reported:
    assert reported[0].startswith("EmulationError("), reported
print("ok")
"""


@pytest.mark.parametrize("kind", ["stop", "raise"])
def test_a_wait_of_an_earlier_free_run_delivers_nothing_of_the_next(kind: str) -> None:
    script = (
        STALE_WAIT.replace("__KIND__", repr(kind))
        .replace("__API__", repr(str(pathlib.Path(__file__).parent)))
        .replace("__JSON__", repr(str(MODEL_JSON)))
        .replace("__ELF__", repr(str(MODEL_ELF)))
    )
    child = run_child(script, timeout=CHILD_TIMEOUT)
    assert child.returncode == 0, child.stderr
    assert child.stdout.strip() == "ok"


TWO_WAITS = """
import asyncio
import gc
import sys
import threading

sys.path.insert(0, __API__)

import popcorn
from models_example import Timer

reported = []
sys.unraisablehook = lambda raised: reported.append(repr(raised.exc_value))
count = [0]
registered = threading.Event()


def handler(call):
    count[0] += 1
    if count[0] == 1:
        assert registered.wait(10)
    if count[0] == 3:
        return call.stop()
    return call.proceed()


def completed(future):
    done = threading.Event()
    future.add_done_callback(done.set)
    assert done.wait(10)
    return future


m = popcorn.Machine(
    popcorn.load_description(__JSON__), image=popcorn.Image.elf(__ELF__), models=dict(TIMER=Timer)
)
m.reset()
asyncio.run(m.run(until=m.sym("ready"), budget=10_000))
asyncio.run(m.call("start_timer", 1, budget=10_000))
m.intercept("TIMER_IRQHandler", handler)
m.start()
boundary_wait = m._handle.wait(None, 1_500_000)
end_wait = m._handle.wait(None, None)
registered.set()
at_boundary = completed(boundary_wait)
at_end = completed(end_wait)
end = at_end.result(0.0)
seen = at_boundary.result(0.0)
assert end.reason == popcorn._core.StopReason.user, (end.reason, end.detail)
assert seen.reason == popcorn._core.StopReason.time, (seen.reason, seen.detail)
del at_end, at_boundary, boundary_wait, end_wait, m
gc.collect()
assert reported == [], reported
print("ok")
"""


def test_an_end_a_wait_delivered_is_not_reported_after_a_boundary_stop_read_later() -> None:
    script = (
        TWO_WAITS.replace("__API__", repr(str(pathlib.Path(__file__).parent)))
        .replace("__JSON__", repr(str(MODEL_JSON)))
        .replace("__ELF__", repr(str(MODEL_ELF)))
    )
    child = run_child(script, timeout=CHILD_TIMEOUT)
    assert child.returncode == 0, child.stderr
    assert child.stdout.strip() == "ok"


def model_machine() -> popcorn.Machine:
    m = popcorn.Machine(
        popcorn.load_description(MODEL_JSON),
        image=popcorn.Image.elf(MODEL_ELF),
        models={"TIMER": Recorder},
    )
    m.reset()
    assert read_count(m) == 7
    return m


def device_machine() -> popcorn.Machine:
    from popcorn.stm32 import mcu

    from .devices_example import Eeprom24
    from .test_devices import I2C_ELF

    m = popcorn.Machine(
        mcu("STM32U575QGIx"),
        image=popcorn.Image.elf(I2C_ELF),
        devices={"I2C1": [Eeprom24(0x50, 256)]},
    )
    m.reset()
    asyncio.run(m.run(budget=10_000))
    return m


@pytest.mark.parametrize("build", [model_machine, device_machine], ids=["model", "device"])
def test_a_context_holding_the_machine_does_not_keep_its_models_alive(
    build: Callable[[], popcorn.Machine], u575: popcorn.MachineDescription
) -> None:
    del u575
    gc.collect()
    before = _core.host_threads()

    def made() -> weakref.ref[popcorn.Machine]:
        cell: list[object] = []
        CURRENT.set(cell)
        m = build()
        cell.append(m)
        assert _core.host_threads() == before + 1
        return weakref.ref(m)

    probe = contextvars.copy_context().run(made)
    gc.collect()
    assert probe() is None
    assert _core.host_threads() == before


READS: list[str] = []


class Counted(Recorder):
    """A Recorder whose CNT reads also count in `READS`, which outlives it."""

    def __init__(self) -> None:
        super().__init__()
        self.keeper: object = None

    @on_read("CNT")
    def _cnt(self) -> int:
        READS.append(VALUE.get())
        return 7


class Holding(Eeprom24):
    """An Eeprom24 that may hold its machine and records the value of VALUE at each start."""

    def __init__(self, address: int, size: int) -> None:
        super().__init__(address, size)
        self.machine: popcorn.Machine | None = None
        self.starts: list[str] = []

    def on_start(self, read: bool, at: VirtualTime) -> bool:
        self.starts.append(VALUE.get())
        return super().on_start(read, at)


def held_model_machine() -> tuple[popcorn.Machine, Recorder | Holding]:
    m = model_machine()
    model = m.model("TIMER")
    assert isinstance(model, Recorder)
    return m, model


def held_device_machine() -> tuple[popcorn.Machine, Recorder | Holding]:
    from popcorn.stm32 import mcu

    from .test_devices import I2C_ELF

    device = Holding(0x50, 256)
    m = popcorn.Machine(
        mcu("STM32U575QGIx"), image=popcorn.Image.elf(I2C_ELF), devices={"I2C1": [device]}
    )
    m.reset()
    asyncio.run(m.run(budget=10_000))
    return m, device


@pytest.mark.parametrize(
    "build", [held_model_machine, held_device_machine], ids=["model", "device"]
)
def test_a_parked_machine_held_by_its_model_is_collected(
    build: Callable[[], tuple[popcorn.Machine, Recorder | Holding]],
    u575: popcorn.MachineDescription,
) -> None:
    del u575
    gc.collect()
    before = _core.host_threads()

    def made() -> weakref.ref[popcorn.Machine]:
        m, held = build()
        held.machine = m
        assert _core.host_threads() == before + 1
        return weakref.ref(m)

    probe = made()
    gc.collect()
    assert probe() is None
    assert _core.host_threads() == before


def test_a_machine_handle_reports_its_python_models_to_the_collector() -> None:
    m, model = held_model_machine()
    handle = m._handle  # pyright: ignore[reportPrivateUsage]
    assert gc.is_tracked(handle)
    assert gc.get_referents(handle).count(model) == 1


def counted_machine() -> popcorn.Machine:
    def build() -> popcorn.Machine:
        VALUE.set("at construction")
        m = popcorn.Machine(
            popcorn.load_description(MODEL_JSON),
            image=popcorn.Image.elf(MODEL_ELF),
            models={"TIMER": Counted},
        )
        m.reset()
        stop = asyncio.run(m.run(until=m.sym("ready"), budget=10_000))
        assert stop.reason is popcorn.StopReason.ADDRESS
        return m

    return contextvars.copy_context().run(build)


def test_a_model_of_a_machine_marked_collected_is_not_called_and_stops_the_run() -> None:
    m = counted_machine()
    READS.clear()
    m._handle.mark_collected()  # pyright: ignore[reportPrivateUsage]
    result = asyncio.run(m.call("read_timer_count_n", 100, budget=100_000))
    assert READS == []
    assert result.stop.reason is popcorn.StopReason.USER
    assert result.stop.detail == COLLECTED


class Keeper:
    """Saves its machine into `SAVED` when collected, resurrecting the garbage it belongs to."""

    def __init__(self, machine: popcorn.Machine) -> None:
        self.machine = machine

    def __del__(self) -> None:
        SAVED.append(self.machine)


SAVED: list[popcorn.Machine] = []


def test_a_resurrected_machine_stops_on_its_first_model_call() -> None:
    cells: list[object] = []

    def build() -> None:
        m = counted_machine()
        cells.append(m._model_context)  # pyright: ignore[reportPrivateUsage]
        model = m.model("TIMER")
        assert isinstance(model, Counted)
        model.keeper = Keeper(m)

    build()
    gc.collect()
    [m] = SAVED
    SAVED.clear()
    assert m._model_context is cells[0]  # pyright: ignore[reportPrivateUsage]
    READS.clear()
    result = asyncio.run(m.call("read_timer_count_n", 100, budget=100_000))
    assert READS == []
    assert result.stop.reason is popcorn.StopReason.USER
    assert result.stop.detail == COLLECTED


class Spy:
    """Forwards to `target`, recording in `calls` each attribute read under `label`."""

    def __init__(self, target: object, label: str, calls: list[str]) -> None:
        self._target = target
        self._label = label
        self._calls = calls

    def __getattr__(self, name: str) -> object:
        self._calls.append(f"{self._label}.{name}")
        return getattr(self._target, name)


def test_a_machine_finalizer_refuses_callbacks_before_it_parks() -> None:
    m = counted_machine()
    calls: list[str] = []
    handle = m._handle  # pyright: ignore[reportPrivateUsage]
    table = m._callback_table  # pyright: ignore[reportPrivateUsage]
    m._handle = Spy(handle, "handle", calls)  # type: ignore[assignment]  # pyright: ignore[reportPrivateUsage]
    m._callback_table = Spy(table, "table", calls)  # type: ignore[assignment]  # pyright: ignore[reportPrivateUsage]
    popcorn.Machine.__del__(m)
    m._handle = handle  # pyright: ignore[reportPrivateUsage]
    m._callback_table = table  # pyright: ignore[reportPrivateUsage]
    assert calls == [
        "table.close",
        "handle.mark_collected",
        "handle.owns_engine",
        "handle.unreported_end",
    ]


def test_a_hook_of_a_machine_found_garbage_raises_hook_gone() -> None:
    m = counted_machine()
    called: list[int] = []
    m.intercept("read_timer_count", lambda call: called.append(1) or call.proceed())
    m._callback_table.close()  # pyright: ignore[reportPrivateUsage]
    with pytest.raises(ReferenceError, match="machine of this hook is gone"):
        read_count(m)
    assert called == []


def test_a_model_of_a_collected_machine_is_not_called_and_stops_the_run() -> None:
    m = counted_machine()
    READS.clear()
    assert read_count(m) == 7
    assert READS == ["at construction"]
    del m._model_context  # pyright: ignore[reportPrivateUsage]
    result = asyncio.run(m.call("read_timer_count_n", 100, budget=100_000))
    assert READS == ["at construction"]
    assert result.stop.reason is popcorn.StopReason.USER
    assert result.stop.detail == COLLECTED


def test_a_device_of_a_collected_machine_is_not_called_and_stops_the_run(
    u575: popcorn.MachineDescription,
) -> None:
    from .test_devices import I2C_ELF, enter

    device = Holding(0x50, 4096)
    m = popcorn.Machine(u575, image=popcorn.Image.elf(I2C_ELF), devices={"I2C1": [device]})
    enter(m, "eeprom_rw")
    del m._model_context  # pyright: ignore[reportPrivateUsage]
    stop = asyncio.run(m.run(budget=100_000_000))
    assert device.starts == []
    assert stop.reason is popcorn.StopReason.USER
    assert stop.detail == COLLECTED


FREE_RUN = """
import asyncio
import contextvars
import gc
import sys
import time
import weakref

sys.path.insert(0, __API__)

import popcorn
from popcorn import _core
from models_example import Timer

unraisable = []
sys.unraisablehook = lambda raised: unraisable.append(repr(raised.exc_value))
VALUE = contextvars.ContextVar("value")
CALLS = []
SHAPE = __SHAPE__


class Holding(Timer):
    keep = None
    cell = None

    def _expire(self):
        dead = self.cell is not None and self.cell() is None
        CALLS.append((VALUE.get("missing"), dead))
        VALUE.get()
        super()._expire()


def build():
    VALUE.set("set")
    m = popcorn.Machine(
        popcorn.load_description(__JSON__),
        image=popcorn.Image.elf(__ELF__),
        models=dict(TIMER=Holding),
    )
    m.reset()
    asyncio.run(m.run(until=m.sym("ready"), budget=10_000))
    asyncio.run(m.call("start_timer", 1, budget=10_000))
    model = m.model("TIMER")
    model.cell = weakref.ref(m._model_context)
    if SHAPE == "attribute":
        model.keep = m
    elif SHAPE == "method":
        model.keep = m.halt
    elif SHAPE == "lambda":
        model.keep = lambda: m
    elif SHAPE == "hook":
        model.keep = m
        m.intercept("TIMER_IRQHandler", lambda call: call.proceed())
    m.start()
    return weakref.ref(m)


for _ in range(__ROUNDS__):
    CALLS.clear()
    probe = build()
    until = time.monotonic() + 10
    while len(CALLS) < 50:
        assert time.monotonic() < until, "the model was never called"
        time.sleep(0.001)
    gc.collect()
    collected = len(CALLS)
    assert probe() is None, "the machine leaked"
    assert _core.host_threads() == 0, _core.host_threads()
    assert len(CALLS) == collected, "a model ran after its machine was collected"
    assert all(value == "set" and not dead for value, dead in CALLS), CALLS[-5:]
    assert not unraisable, unraisable
print("ok")
"""


def free_run_script(shape: str, rounds: int) -> str:
    return (
        FREE_RUN.replace("__API__", repr(str(pathlib.Path(__file__).parent)))
        .replace("__SHAPE__", repr(shape))
        .replace("__JSON__", repr(str(MODEL_JSON)))
        .replace("__ELF__", repr(str(MODEL_ELF)))
        .replace("__ROUNDS__", str(rounds))
    )


@pytest.mark.parametrize("shape", ["attribute", "method", "lambda", "hook"])
def test_a_free_running_machine_held_by_its_model_is_collected(shape: str) -> None:
    child = subprocess.run(
        [sys.executable, "-X", "dev", "-c", free_run_script(shape, 5)],
        capture_output=True,
        text=True,
        timeout=CHILD_TIMEOUT,
        check=False,
    )
    assert child.returncode == 0, child.stderr
    assert child.stdout.strip() == "ok"


def test_a_collection_inside_a_handler_collects_a_free_running_cycle(
    machines: Machines,
) -> None:
    gc.collect()
    before = _core.host_threads()
    first = machines()

    def other() -> weakref.ref[popcorn.Machine]:
        m, model = held_model_machine()
        model.machine = m
        asyncio.run(m.call("start_timer", 1, budget=10_000))
        m.start()
        return weakref.ref(m)

    probe = other()
    collected: list[int] = []

    def handler(call: popcorn.Call) -> popcorn.Return:
        collected.append(gc.collect())
        return call.proceed()

    first.intercept("alu_mark", handler)
    asyncio.run(first.run(budget=1_000_000))
    assert len(collected) == 1 and collected[0] > 0
    assert probe() is None
    assert settles(lambda: _core.host_threads() == before + 1)


def test_an_interpreter_exits_with_a_free_running_machine_held_by_its_model() -> None:
    child = run_child(
        f"""
        import asyncio
        import popcorn
        import sys

        sys.path.insert(0, {str(pathlib.Path(__file__).parent)!r})
        from models_example import Timer

        m = popcorn.Machine(
            popcorn.load_description({str(MODEL_JSON)!r}),
            image=popcorn.Image.elf({str(MODEL_ELF)!r}),
            models=dict(TIMER=Timer),
        )
        m.reset()
        asyncio.run(m.run(until=m.sym("ready"), budget=10_000))
        asyncio.run(m.call("start_timer", 1, budget=10_000))
        m.model("TIMER").machine = m
        m.start()
        """,
        timeout=10,
    )
    assert child.returncode == 0, child.stderr


ABANDONED = """
import asyncio
import gc
import os
import signal
import sys
import threading
import time

import popcorn
from popcorn import _core

MODE = __MODE__
released = threading.Event()
entered = threading.Event()
caught = threading.Event()


def blocking(call):
    entered.set()
    released.wait()
    return call.proceed()


cancelled_first = []


def interrupt():
    deadline = time.monotonic() + 10
    while not caught.is_set() and time.monotonic() < deadline:
        os.kill(os.getpid(), signal.SIGINT)
        time.sleep(0.2)
        if not cancelled_first and running_task:
            cancelled_first.append(running_task[0].cancelling() > 0 and not pending[0].done())


m = popcorn.Machine(popcorn.load_description(__JSON__), image=popcorn.Image.elf(__ELF__))
m.reset()
m.intercept("alu_mark", blocking)
before = _core.host_threads()
pending = []


running_task = []


async def goal():
    running = asyncio.create_task(m.run(budget=10_000_000))
    running_task.append(asyncio.current_task())
    while not entered.is_set():
        await asyncio.sleep(0.01)
    pending.append(m.read(m.ref.pc))
    threading.Thread(target=interrupt, daemon=True).start()
    await running


def interrupt_inside(entered_before):
    deadline = time.monotonic() + 10
    while _core.interruptible_waits() == entered_before and time.monotonic() < deadline:
        time.sleep(0.001)
    interrupt()


def waiting_on_result():
    return future.result(None)


try:
    if MODE == "asyncio":
        asyncio.run(goal())
    elif MODE == "result":
        future = m._handle.run_goal(None, 10_000_000, None, None, None)
        assert entered.wait(10)
        threading.Thread(
            target=interrupt_inside, args=(_core.interruptible_waits(),), daemon=True
        ).start()
        waiting_on_result()
    else:
        m.start()
        assert entered.wait(10)
        threading.Thread(
            target=interrupt_inside, args=(_core.interruptible_waits(),), daemon=True
        ).start()
        m.halt()
except KeyboardInterrupt:
    caught.set()
else:
    raise AssertionError("no KeyboardInterrupt")

if MODE == "asyncio":
    assert cancelled_first == [True], cancelled_first
for posted in pending:
    try:
        asyncio.run(posted)
    except popcorn.EmulationError as error:
        assert "closed" in str(error), error
    else:
        raise AssertionError("a pending command survived the abandon")
try:
    m.mem.u32[0x2000_8000]
except popcorn.EmulationError as error:
    assert "closed" in str(error), error
else:
    raise AssertionError("an access after the abandon")
assert m._park() is None
assert m.halt() is None
assert not m.busy
released.set()
deadline = time.monotonic() + 10
while _core.host_threads() > before and time.monotonic() < deadline:
    time.sleep(0.01)
assert _core.host_threads() == before
try:
    m.mem.u32[0x2000_8000]
except popcorn.EmulationError as error:
    assert "closed" in str(error), error
del m
gc.collect()
print("ok")
"""


@pytest.mark.parametrize("mode", ["asyncio", "result", "halt"])
def test_ctrl_c_on_a_blocked_handler_abandons_its_machine(mode: str) -> None:
    script = (
        ABANDONED.replace("__MODE__", repr(mode))
        .replace("__JSON__", repr(str(M33_JSON)))
        .replace("__ELF__", repr(str(DRIVER_ELF)))
    )
    child = run_child(script, timeout=20)
    assert child.returncode == 0, child.stderr
    assert child.stdout.strip() == "ok"


BLOCKED_AT_EXIT = """
import os
import signal
import threading
import time

import popcorn

entered = threading.Event()


def blocking(call):
    entered.set()
    threading.Event().wait()
    return call.proceed()


m = popcorn.Machine(popcorn.load_description(__JSON__), image=popcorn.Image.elf(__ELF__))
m.reset()
m.intercept("alu_mark", blocking)
m.start()
assert entered.wait(10)


def interrupt_later():
    time.sleep(1.0)
    os.kill(os.getpid(), signal.SIGINT)


if __SIGNAL__:
    threading.Thread(target=interrupt_later, daemon=True).start()
print("leaving", flush=True)
"""


def test_a_chained_goal_error_raises_the_finish_error_with_the_end_as_context() -> None:
    with pytest.raises(popcorn.PopcornError, match="finish failed") as caught:
        _core.raise_chained_error()
    context = caught.value.__context__
    assert isinstance(context, popcorn.EmulationError)
    assert "cancelled" in str(context)


def test_a_finish_failing_after_a_handler_error_chains_both(machines: Machines) -> None:
    m = machines()

    def raising(call: popcorn.Call) -> popcorn.Return:
        raise ValueError("the handler failed")

    m.intercept("alu_mark", raising)
    future = m._handle.failing_finish_goal(10_000_000)  # pyright: ignore[reportPrivateUsage]
    with pytest.raises(popcorn.PopcornError, match="finish failed") as caught:
        future.result(None)
    context = caught.value.__context__
    assert isinstance(context, ValueError)
    assert str(context) == "the handler failed"


def test_a_finish_failing_after_a_cancel_is_reported_and_the_task_sees_the_cancel(
    machines: Machines, monkeypatch: pytest.MonkeyPatch
) -> None:
    m = machines()
    reported: list[BaseException | None] = []

    def report(raised: sys.UnraisableHookArgs) -> None:
        reported.append(raised.exc_value)

    monkeypatch.setattr(sys, "unraisablehook", report)
    entered = threading.Event()
    released = threading.Event()

    def blocking(call: popcorn.Call) -> popcorn.Return:
        entered.set()
        released.wait()
        return call.proceed()

    m.intercept("alu_mark", blocking, once=True)

    async def scenario() -> None:
        future = m._handle.failing_finish_goal(10_000_000)  # pyright: ignore[reportPrivateUsage]
        task = asyncio.create_task(awaited(future, lambda raw: raw))
        while not entered.is_set():
            await asyncio.sleep(0.001)
        task.cancel()
        released.set()
        with pytest.raises(asyncio.CancelledError):
            await task

    asyncio.run(scenario())
    assert len(reported) == 1
    raised = reported[0]
    assert isinstance(raised, popcorn.PopcornError)
    assert "finish failed" in str(raised)
    assert isinstance(raised.__context__, asyncio.CancelledError)


def test_a_late_command_failure_is_reported_without_the_gil_on_the_machine_thread(
    machines: Machines, monkeypatch: pytest.MonkeyPatch
) -> None:
    """The read of unmapped memory fails on the machine's thread at its stamp, one virtual
    second ahead, after the awaiting task was cancelled."""
    m = machines()
    at = m.time + s(1)
    unmapped = m.ref.mem.u32[0xF000_0000].read_op()
    reported: list[tuple[str | None, type[object], list[str]]] = []

    def report(raised: sys.UnraisableHookArgs) -> None:
        notes = getattr(raised.exc_value, "__notes__", [])
        reported.append((raised.err_msg, type(raised.exc_value), list(notes)))

    monkeypatch.setattr(sys, "unraisablehook", report)
    entries = m._handle.gil_entries()  # pyright: ignore[reportPrivateUsage]
    submit = m._handle.submit_ops  # pyright: ignore[reportPrivateUsage]

    async def scenario() -> None:
        async with m.running():
            pending: Pending[object] = Pending(
                submit([unmapped], ["reading the probe"], at.ps), lambda raw: raw, cancel=False
            )
            task = asyncio.create_task(pending)
            del pending
            await asyncio.sleep(0)
            task.cancel()
            with pytest.raises(asyncio.CancelledError):
                await task
            for _ in range(500):
                if reported:
                    break
                await asyncio.sleep(0.01)

    asyncio.run(scenario())
    assert m._handle.gil_entries() == entries  # pyright: ignore[reportPrivateUsage]
    assert reported == [
        (
            "a command its awaiter cancelled failed later",
            popcorn.EmulationError,
            ["reading the probe"],
        )
    ]


@pytest.mark.parametrize("later", ["run", "collect"])
def test_a_late_failure_after_its_loop_ended_is_reported_once(
    monkeypatch: pytest.MonkeyPatch, later: str
) -> None:
    m = popcorn.Machine(
        popcorn.load_description(str(M33_JSON)), image=popcorn.Image.elf(str(DRIVER_ELF))
    )
    m.reset()
    at = m.time + s(1)
    unmapped = m.ref.mem.u32[0xF000_0000].read_op()
    reported: list[str | None] = []

    def report(raised: sys.UnraisableHookArgs) -> None:
        reported.append(raised.err_msg)

    monkeypatch.setattr(sys, "unraisablehook", report)
    post = m._post  # pyright: ignore[reportPrivateUsage]

    async def scenario() -> None:
        task = asyncio.create_task(post(unmapped, at, cancel=False))
        await asyncio.sleep(0)
        task.cancel()
        with pytest.raises(asyncio.CancelledError):
            await task

    asyncio.run(scenario())
    assert reported == []
    if later == "run":
        asyncio.run(m.run(for_=s(2)))
    else:
        del m, post, scenario
        gc.collect()
    assert reported == ["a command its awaiter cancelled failed later"]


@pytest.mark.parametrize("a_first", [True, False], ids=["a_first", "b_first"])
def test_an_access_to_a_machine_held_by_another_thread_waits_for_it(
    machines: Machines, a_first: bool
) -> None:
    first = machines()
    second = machines()
    a, b = (first, second) if a_first else (second, first)
    held = threading.Event()
    release = threading.Event()

    def hold() -> None:
        held.set()
        release.wait()

    holder = threading.Thread(
        target=lambda: b._handle.parked(hold)  # pyright: ignore[reportPrivateUsage]
    )
    holder.start()
    assert held.wait(10)
    releaser = threading.Timer(0.1, release.set)
    releaser.start()
    began = time.monotonic()
    read = a._handle.parked(lambda: b.mem.u32[0x2000_8000])  # pyright: ignore[reportPrivateUsage]
    took = time.monotonic() - began
    holder.join(10)
    releaser.join(10)
    assert isinstance(read, int)
    assert took >= 0.05


def test_crossed_accesses_refuse_one_wait_naming_both_machines(machines: Machines) -> None:
    a = machines()
    b = machines()
    names = {
        f"#{a._handle.id()}",  # pyright: ignore[reportPrivateUsage]
        f"#{b._handle.id()}",  # pyright: ignore[reportPrivateUsage]
    }
    crossing = threading.Barrier(2)
    outcomes: dict[str, object] = {}

    def access(into: popcorn.Machine, key: str) -> None:
        crossing.wait(10)
        try:
            outcomes[key] = into.mem.u32[0x2000_8000]
        except popcorn.MachineRunning as refused:
            outcomes[key] = refused

    other = threading.Thread(
        target=lambda: b._handle.parked(  # pyright: ignore[reportPrivateUsage]
            lambda: access(a, "other")
        )
    )
    other.start()
    a._handle.parked(lambda: access(b, "main"))  # pyright: ignore[reportPrivateUsage]
    other.join(10)
    refused = [value for value in outcomes.values() if isinstance(value, popcorn.MachineRunning)]
    landed = [value for value in outcomes.values() if isinstance(value, int)]
    assert (len(refused), len(landed)) == (1, 1)
    assert all(name in str(refused[0]) for name in names)


class _Recorder:
    """Forwards to `wrapped`, recording the named calls in `calls`."""

    def __init__(self, wrapped: object, label: str, names: set[str], calls: list[str]) -> None:
        self._wrapped = wrapped
        self._label = label
        self._names = names
        self._calls = calls

    def __getattr__(self, name: str) -> object:
        found = getattr(self._wrapped, name)
        if name not in self._names:
            return found
        calls, label = self._calls, self._label

        def recorded(*args: object) -> object:
            calls.append(f"{label}.{name}")
            return found(*args)

        return recorded


@pytest.mark.parametrize("collected", [False, True], ids=["called", "collected"])
def test_a_collected_machine_closes_its_callbacks_then_marks_then_parks(
    machines: Machines, collected: bool
) -> None:
    m = (
        popcorn.Machine(
            popcorn.load_description(str(M33_JSON)), image=popcorn.Image.elf(str(DRIVER_ELF))
        )
        if collected
        else machines()
    )
    calls: list[str] = []
    spied: Any = m
    table, handle = spied._callback_table, spied._handle
    spied._callback_table = _Recorder(table, "table", {"close"}, calls)
    spied._handle = _Recorder(handle, "handle", {"mark_collected", "unreported_end"}, calls)
    if collected:
        del m, spied, table, handle
        gc.collect()
    else:
        m.__del__()
        spied._callback_table, spied._handle = table, handle
    assert calls == ["table.close", "handle.mark_collected", "handle.unreported_end"]


class _Probe:
    def __call__(self, view: object) -> object:
        raise AssertionError("never called")


@pytest.mark.parametrize("a_first", [True, False], ids=["a_first", "b_first"])
def test_a_hook_dropped_while_its_host_is_held_goes_at_the_next_parked_access(
    machines: Machines, a_first: bool
) -> None:
    if a_first:
        a = machines()
        b = machines()
    else:
        b = machines()
        a = machines()
    probe = _Probe()
    gone = weakref.ref(probe)
    hook: object = a._handle.add_intercept(  # pyright: ignore[reportPrivateUsage]
        a.address_of("idle_main"), probe, False
    )
    del probe
    held = threading.Event()
    release = threading.Event()

    def hold() -> None:
        held.set()
        release.wait()

    holder = threading.Thread(
        target=lambda: a._handle.parked(hold)  # pyright: ignore[reportPrivateUsage]
    )
    holder.start()
    assert held.wait(10)

    def drop() -> None:
        nonlocal hook
        hook = None
        gc.collect()

    b._handle.parked(drop)  # pyright: ignore[reportPrivateUsage]
    release.set()
    holder.join(10)
    gc.collect()
    assert gone() is not None
    assert a.mem.u32[0x2000_8000] is not None
    gc.collect()
    assert gone() is None


ABANDONED_LATE = """
import asyncio
import gc
import os
import signal
import sys
import threading
import time

import popcorn
from popcorn import _core

released = threading.Event()
entered = threading.Event()


def blocking(call):
    entered.set()
    released.wait()
    return call.proceed()


m = popcorn.Machine(popcorn.load_description(__JSON__), image=popcorn.Image.elf(__ELF__))
m.reset()
m.intercept("alu_mark", blocking)
before = _core.host_threads()
m.start()
assert entered.wait(10)


async def leave():
    task = asyncio.create_task(m._post(None, cancel=False))
    await asyncio.sleep(0)
    task.cancel()
    try:
        await task
    except asyncio.CancelledError:
        pass


asyncio.run(leave())


def interrupt_inside(entered_before):
    deadline = time.monotonic() + 10
    while _core.interruptible_waits() == entered_before and time.monotonic() < deadline:
        time.sleep(0.001)
    os.kill(os.getpid(), signal.SIGINT)


threading.Thread(target=interrupt_inside, args=(_core.interruptible_waits(),), daemon=True).start()
try:
    m.halt()
except KeyboardInterrupt:
    pass
else:
    raise AssertionError("no KeyboardInterrupt")
reported = []
sys.unraisablehook = lambda raised: reported.append(raised.err_msg)
released.set()
deadline = time.monotonic() + 10
while _core.host_threads() > before and time.monotonic() < deadline:
    time.sleep(0.01)
assert reported == [], reported
if __NATIVE_ONLY__:
    popcorn.Machine.__del__ = lambda self: None
del m
gc.collect()
print(reported)
"""


@pytest.mark.parametrize("native_only", [False, True], ids=["finalizer", "native"])
def test_a_late_failure_of_an_abandoned_machine_is_reported_when_it_is_dropped(
    native_only: bool,
) -> None:
    """The native variant skips the finalizer's flush, as at a collection on the machine's
    own thread, so only the handle's destructor can report."""
    script = (
        ABANDONED_LATE.replace("__NATIVE_ONLY__", repr(native_only))
        .replace("__JSON__", repr(str(M33_JSON)))
        .replace("__ELF__", repr(str(DRIVER_ELF)))
    )
    child = run_child(script, timeout=30)
    assert child.returncode == 0, child.stderr
    assert child.stdout.strip() == "['a command its awaiter cancelled failed later']"


ORPHAN_REMOVALS = """
import gc
import os
import signal
import threading
import time
import weakref

import popcorn
from popcorn import _core

released = threading.Event()
entered = threading.Event()


def blocking(call):
    entered.set()
    released.wait()
    return call.proceed()


class Probe:
    def __call__(self, view):
        raise AssertionError("never called")


m = popcorn.Machine(popcorn.load_description(__JSON__), image=popcorn.Image.elf(__ELF__))
m.reset()
m.intercept("alu_mark", blocking)
probe = Probe()
gone = weakref.ref(probe)
first = m._handle.add_intercept(m.address_of("idle_main"), probe, False)
spares = [m._handle.add_intercept(m.address_of("bkpt_main"), Probe(), False) for _ in range(50)]
del probe
before = _core.host_threads()


def interrupt_inside(entered_before):
    deadline = time.monotonic() + 10
    while _core.interruptible_waits() == entered_before and time.monotonic() < deadline:
        time.sleep(0.001)
    os.kill(os.getpid(), signal.SIGINT)


m.start()
assert entered.wait(10)
threading.Thread(target=interrupt_inside, args=(_core.interruptible_waits(),), daemon=True).start()
try:
    m.halt()
except KeyboardInterrupt:
    pass
else:
    raise AssertionError("no KeyboardInterrupt")
del first
gc.collect()
assert gone() is not None, "a removal ran while the abandoned thread lives"
released.set()
deadline = time.monotonic() + 10
while _core.host_threads() > before and time.monotonic() < deadline:
    time.sleep(0.01)
while spares and gone() is not None:
    spares.pop()
    gc.collect()
    time.sleep(0.01)
assert gone() is None, "the queued removal did not run once the thread was gone"
print("ok")
"""


def test_an_abandoned_machine_runs_its_queued_removals_once_its_thread_is_gone() -> None:
    script = ORPHAN_REMOVALS.replace("__JSON__", repr(str(M33_JSON))).replace(
        "__ELF__", repr(str(DRIVER_ELF))
    )
    child = run_child(script, timeout=30)
    assert child.returncode == 0, child.stderr
    assert child.stdout.strip() == "ok"


DROPPED_AFTER_EXIT = """
import atexit
import gc

kept = {}


def drop():
    assert kept["machine"].halt() is None
    kept.clear()
    gc.collect()


atexit.register(drop)

import threading

import popcorn

entered = threading.Event()


def blocking(call):
    entered.set()
    threading.Event().wait()
    return call.proceed()


m = popcorn.Machine(popcorn.load_description(__JSON__), image=popcorn.Image.elf(__ELF__))
m.reset()
m.intercept("alu_mark", blocking)
m.start()
assert entered.wait(10)
kept["machine"] = m
kept["cycle"] = kept
del m
print("leaving", flush=True)
"""


def test_a_machine_halted_and_dropped_after_the_exit_abandon_does_not_hang() -> None:
    script = DROPPED_AFTER_EXIT.replace("__JSON__", repr(str(M33_JSON))).replace(
        "__ELF__", repr(str(DRIVER_ELF))
    )
    child = run_child(script, timeout=30)
    assert child.stdout.strip() == "leaving"
    assert "the machine thread of POPCORN-M33-TEST did not stop at exit" in child.stderr


@pytest.mark.parametrize("interrupted", [False, True], ids=["timeout", "signal"])
def test_an_exit_with_a_blocked_handler_abandons_its_machine(interrupted: bool) -> None:
    script = (
        BLOCKED_AT_EXIT.replace("__SIGNAL__", repr(interrupted))
        .replace("__JSON__", repr(str(M33_JSON)))
        .replace("__ELF__", repr(str(DRIVER_ELF)))
    )
    began = time.monotonic()
    child = run_child(script, timeout=30)
    took = time.monotonic() - began
    assert child.stdout.strip() == "leaving"
    assert "the machine thread of POPCORN-M33-TEST did not stop at exit" in child.stderr
    assert took < (5 if interrupted else 15)


def test_no_binding_runs_a_slice_directly() -> None:
    assert not hasattr(_core.MachineHandle, "run_slice")
    assert not hasattr(_core, "SliceArgs")


def test_a_body_error_leaving_running_wins_over_a_kept_log_error() -> None:
    m = timer_started_in_a_free_run()
    failing = FailOnce()
    logger = logging.getLogger("popcorn")

    async def scenario() -> None:
        async with m.running():
            logger.addFilter(failing)
            time.sleep(0.05)
            m._flush_logs()  # pyright: ignore[reportPrivateUsage]
            assert failing.failed
            raise asyncio.CancelledError("the body")

    try:
        with pytest.raises(asyncio.CancelledError, match="the body") as caught:
            asyncio.run(scenario())
    finally:
        logger.removeFilter(failing)
    assert any("filter failed" in note for note in caught.value.__notes__)
    assert not m.busy


class InterruptOnce(logging.Handler):
    """Raises KeyboardInterrupt on the first record it handles, records the others."""

    def __init__(self) -> None:
        super().__init__()
        self.interrupted = False
        self.seen: list[str] = []

    def emit(self, record: logging.LogRecord) -> None:
        if not self.interrupted:
            self.interrupted = True
            raise KeyboardInterrupt
        self.seen.append(getattr(record, "popcorn_register", ""))


def test_a_ctrl_c_in_a_log_handler_propagates_and_keeps_its_record() -> None:
    m = timer_started_in_a_free_run()
    handler = InterruptOnce()
    logger = logging.getLogger("popcorn")
    logger.addHandler(handler)
    try:
        m.start()
        with pytest.raises(KeyboardInterrupt):
            asyncio.run(m.wait_for(never(m), for_=us(50)))
        assert m.halt() is None
    finally:
        logger.removeHandler(handler)
    assert handler.seen == ["ARR", "CR"]


def test_a_park_after_a_wait_returned_the_end_returns_none(machines: Machines) -> None:
    m = machines()

    class Never:
        def ready(self, machine: popcorn.Machine) -> bool:
            return False

    m.intercept("alu_mark", lambda call: call.stop())
    m.start()
    stop = asyncio.run(m.wait_for(Never()))
    assert stop.reason is popcorn.StopReason.USER
    assert m._park() is None  # pyright: ignore[reportPrivateUsage]


def test_an_always_raising_filter_sees_the_dropped_count_a_bounded_number_of_times(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """5000 records overflow the 4096 bound. Each flush the filter breaks ends there and drops
    what it was forwarding, the dropped-count record included, so the loop settles."""
    from popcorn.stm32 import mcu

    def ignore(raised: sys.UnraisableHookArgs) -> None:
        del raised

    monkeypatch.setattr(sys, "unraisablehook", ignore)
    description = mcu("STM32U575QGIx")
    m = popcorn.Machine(description, image=popcorn.Image.elf(BOARD_ELF))
    m.reset()
    holes = gpio_holes(description, 5000)
    calls = {"all": 0, "dropped": 0}

    class Raising(logging.Filter):
        def filter(self, record: logging.LogRecord) -> bool:
            calls["all"] += 1
            if "were dropped" in record.getMessage():
                calls["dropped"] += 1
            raise RuntimeError("filter")

    def touch(machine: popcorn.Machine) -> None:
        for hole in holes:
            machine.mem.u8[hole]

    raising = Raising()
    logger = logging.getLogger("popcorn")
    logger.addFilter(raising)

    async def scenario(machine: popcorn.Machine) -> None:
        with contextlib.suppress(RuntimeError):
            await machine.run(for_=us(200))
        settled = -1
        while settled != calls["all"]:
            settled = calls["all"]
            await asyncio.sleep(0.1)

    try:
        m._handle.parked(functools.partial(touch, m))  # pyright: ignore[reportPrivateUsage]
        asyncio.run(asyncio.wait_for(scenario(m), 20))
    finally:
        logger.removeFilter(raising)
    with contextlib.suppress(RuntimeError):
        m.halt()
    del m
    gc.collect()
    assert calls["dropped"] <= 1
    assert calls["all"] <= 5001


def test_an_internal_wait_raises_the_free_runs_own_end_and_delivers_it(
    callee: popcorn.Machine,
) -> None:
    callee.intercept("SysTick_Handler", lambda call: call.stop())
    never = callee._handle.predicate_pc_at(0xFFFF_FFF0)  # pyright: ignore[reportPrivateUsage]

    async def scenario() -> None:
        callee.start()
        await callee._wait_until(never, None)  # pyright: ignore[reportPrivateUsage]

    with pytest.raises(popcorn.EmulationError, match="free run, while waiting") as raised:
        asyncio.run(scenario())
    assert raised.value.reason is popcorn.StopReason.USER
    assert callee._park() is None  # pyright: ignore[reportPrivateUsage]
