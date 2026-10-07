"""The native host under the binding: callbacks on the machine's thread, in captured contexts,
the future bridge, destruction, shutdown and the lock order."""

from __future__ import annotations

import asyncio
import contextvars
import gc
import pathlib
import subprocess
import sys
import textwrap
import threading
import time
import weakref
from collections.abc import Callable

import popcorn
import pytest
from popcorn import _core
from popcorn._host import awaited
from popcorn._stop import Stop, stop_of
from popcorn.model import Model, on_read
from popcorn.units import VirtualTime

from .conftest import DRIVER_ELF, M33_JSON, Machines
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

KIND, SURFACED, NATIVE = __CASE__
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
m.start()
if SURFACED:
    try:
        assert asyncio.run(m.wait_for(Never())).reason is popcorn.StopReason.USER
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
print("ok")
"""


@pytest.mark.parametrize("native", [False, True], ids=["del", "destructor"])
@pytest.mark.parametrize("surfaced", [False, True], ids=["unclaimed", "surfaced"])
@pytest.mark.parametrize("kind", ["stop", "raise"])
def test_a_collected_machine_reports_only_an_end_no_wait_delivered(
    kind: str, surfaced: bool, native: bool
) -> None:
    script = (
        SELF_ENDED.replace("__CASE__", repr((kind, surfaced, native)))
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
