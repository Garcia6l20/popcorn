"""`Machine.close()` and the close of every live machine at interpreter exit: a Machine held by a
module global leaves no nanobind leak report, and a closed machine refuses every use."""

from __future__ import annotations

import asyncio
import os
import subprocess
import sys
import textwrap
import threading
import time
import weakref
from pathlib import Path
from typing import Any

import popcorn
import pytest
from popcorn import _core
from popcorn._machine import Machine
from popcorn._refs import Pending
from popcorn.model import Model, on_read
from popcorn.units import ms, us

from .conftest import DRIVER_ELF, M33_JSON, Machines, collected, quiet_host_threads, settled
from .projects import MODEL_ELF

TESTS = Path(__file__).parents[1]
I2C_ELF = Path(__file__).parents[2] / "firmware" / "periph" / "prebuilt" / "i2c_ll.elf"
KEPT = {
    "__weakref__",
    "_name",
    "_system",
    "_on_reset_request",
    "_strict_peripherals",
    "_config_digest",
}

MODEL_MACHINE = f"""
import asyncio
import sys

sys.path.insert(0, {str(TESTS)!r})
import popcorn
from derived_data import model_json
from popcorn.model import Model, on_read


class Idle(Model):
    @on_read("CNT")
    def _cnt(self) -> int:
        __CALLBACK__


m = popcorn.Machine(
    popcorn.load_description(model_json()),
    image=popcorn.Image.elf({str(MODEL_ELF)!r}),
    models={{"TIMER": Idle}},
)
m.reset()
asyncio.run(m.run(until=m.sym("ready"), budget=10_000))
"""
RETURNS = "return 7"
RAISES = 'raise RuntimeError("boom")'
CALL = """
try:
    asyncio.run(m.call("read_timer_count", budget=100_000))
except RuntimeError:
    pass
"""


def child(script: str, timeout: float = 30) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, "-c", textwrap.dedent(script)],
        capture_output=True,
        text=True,
        timeout=timeout,
        check=False,
        env={**os.environ, "POPCORN_TEST_EXIT_WAIT_MS": ""},
    )


def clean_exit(script: str) -> None:
    done = child(script)
    assert done.returncode == 0, done.stderr
    assert done.stderr == "", done.stderr


def model_script(callback: str, tail: str = "") -> str:
    return MODEL_MACHINE.replace("__CALLBACK__", callback) + tail


def test_a_global_machine_with_a_python_model_leaves_no_leak_report_at_exit() -> None:
    clean_exit(model_script(RETURNS))


def test_the_same_after_a_model_callback_raised_and_was_caught() -> None:
    clean_exit(model_script(RAISES, CALL))


def test_the_same_after_close() -> None:
    clean_exit(model_script(RETURNS, "\nm.close()\n"))


def test_a_global_machine_with_a_raising_python_device_leaves_no_leak_report() -> None:
    clean_exit(f"""
        import asyncio
        import sys

        sys.path.insert(0, {str(Path(__file__).parent)!r})
        import popcorn
        from popcorn.stm32 import mcu
        from devices_example import Eeprom24


        class Flaky(Eeprom24):
            def __init__(self) -> None:
                super().__init__(0x50, 4096)
                self.n = 0

            def on_start(self, read, at):
                self.n += 1
                if self.n == 3:
                    raise RuntimeError("boom")
                return super().on_start(read, at)


        m = popcorn.Machine(
            mcu("STM32U575QGIx"),
            image=popcorn.Image.elf({str(I2C_ELF)!r}),
            devices={{"I2C1": [Flaky()]}},
        )
        m.reset()
        asyncio.run(m.run(until="main", budget=100_000_000))
        m.reg.pc = m.sym("eeprom_rw")
        try:
            asyncio.run(m.run(budget=100_000_000))
        except RuntimeError:
            pass
        """)


def test_a_started_global_machine_leaves_no_leak_report_at_exit() -> None:
    clean_exit(model_script(RETURNS, "\nm.start()\n"))


def test_a_forked_child_exits_quietly() -> None:
    clean_exit(f"""
        import os
        import sys
        import warnings

        import popcorn

        warnings.simplefilter("ignore", DeprecationWarning)
        m = popcorn.Machine(
            popcorn.load_description({str(M33_JSON)!r}),
            image=popcorn.Image.elf({str(DRIVER_ELF)!r}),
        )
        m.reset()
        asyncio_run = __import__("asyncio").run
        asyncio_run(m.run(budget=1_000))
        pid = os.fork()
        if pid == 0:
            sys.exit(0)
        _, status = os.waitpid(pid, 0)
        assert status == 0, status
        """)


def test_a_handler_registered_after_the_import_finds_its_machine_open() -> None:
    done = child(f"""
        import asyncio
        import atexit
        import sys

        import popcorn

        sys.path.insert(0, {str(TESTS)!r})
        holder = []


        def at_exit() -> None:
            print("time", holder[0].time.ps)


        atexit.register(at_exit)
        m = popcorn.Machine(
            popcorn.load_description({str(M33_JSON)!r}),
            image=popcorn.Image.elf({str(DRIVER_ELF)!r}),
        )
        m.reset()
        holder.append(m)
        """)
    assert done.returncode == 0, done.stderr
    assert done.stdout.startswith("time "), done.stdout
    assert done.stderr == "", done.stderr


def test_a_member_of_a_system_is_left_to_its_system_at_exit() -> None:
    done = child(f"""
        import asyncio
        import sys

        sys.path.insert(0, {str(TESTS)!r})
        import popcorn
        from api.linked import member
        from popcorn import System, UartLink
        from popcorn.stm32 import mcu
        from popcorn.units import us

        u575 = mcu("STM32U575QGIx")
        a = member(u575, "idle", "a")
        b = member(u575, "tx_irq", "b")
        link = UartLink("a.USART1", "b.USART1", latency=us(20), baud=460_800)
        system = System({{"a": a, "b": b}}, [link])
        asyncio.run(system.run(for_=us(100)))
        """)
    assert done.returncode == 0, done.stderr
    assert "closed" not in done.stderr


class Counted(Model):
    reads = 0

    @on_read("CNT")
    def _cnt(self) -> int:
        type(self).reads += 1
        return 7


def counted_machine() -> Machine:
    from derived_data import model_json

    m = popcorn.Machine(
        popcorn.load_description(model_json()),
        image=popcorn.Image.elf(MODEL_ELF),
        models={"TIMER": Counted},
    )
    m.reset()
    asyncio.run(m.run(until=m.sym("ready"), budget=10_000))
    return m


def test_close_is_idempotent_and_a_closed_machine_refuses_every_use(machines: Machines) -> None:
    m = machines()
    m.close()
    m.close()
    assert m.name
    assert "Machine(" in repr(m)
    for use in (
        lambda: m.reg.pc,
        lambda: m.mem.u32[0],
        lambda: m.periph.GPIOA,
        lambda: m.busy,
        lambda: asyncio.run(m.run(budget=10)),
        lambda: m.halt(),
        lambda: m.time,
    ):
        with pytest.raises(popcorn.EmulationError, match="the machine was closed"):
            use()


def test_every_slot_but_the_kept_ones_is_cleared_by_close(machines: Machines) -> None:
    m = machines()
    m.close()
    names = {slot for slot in Machine.__slots__}
    assert names >= KEPT
    for slot in names - KEPT:
        with pytest.raises(popcorn.EmulationError, match="the machine was closed"):
            getattr(m, slot).anything  # noqa: B018
    assert type(object.__getattribute__(m, "_handle")).__name__ == "_Closed"


def test_close_refuses_a_busy_machine_and_leaves_it_working(machines: Machines) -> None:
    m = machines()
    m.start()
    try:
        with pytest.raises(popcorn.MachineRunning):
            m.close()
        assert m.busy
    finally:
        m.halt()
    m.close()


def test_close_from_the_machine_s_own_handler_is_refused(machines: Machines) -> None:
    m = machines()
    seen: list[str] = []

    def handler(call: popcorn.Call) -> popcorn.Return:
        try:
            m.close()
        except popcorn.MachineRunning:
            seen.append("own")
        return call.proceed()

    m.intercept("alu_mark", handler)
    asyncio.run(m.run(budget=1_000_000))
    assert seen == ["own"]
    m.close()


def test_close_from_a_handler_of_another_machine_is_refused(machines: Machines) -> None:
    first, second = machines(), machines()
    seen: list[str] = []

    def handler(call: popcorn.Call) -> popcorn.Return:
        try:
            second.close()
        except popcorn.MachineRunning:
            seen.append("other")
        return call.proceed()

    first.intercept("alu_mark", handler)
    asyncio.run(first.run(budget=1_000_000))
    assert seen == ["other"]
    second.close()
    first.close()


def test_close_refuses_a_member_of_a_system(u575: popcorn.PartDescription) -> None:
    from .linked import member

    a, b = member(u575, "idle", "a"), member(u575, "tx_irq", "b")
    system = popcorn.System(
        {"a": a, "b": b},
        [popcorn.UartLink("a.USART1", "b.USART1", latency=us(20), baud=460_800)],
    )
    with pytest.raises(popcorn.ConfigError, match=r"members\.a"):
        a.close()
    system.close()


def test_close_releases_the_model_the_device_and_the_handler(machines: Machines) -> None:
    kept = counted_machine()
    model = kept.model("TIMER")
    gone_model = weakref.ref(model)
    del model

    def handler(call: popcorn.Call) -> popcorn.Return:
        return call.proceed()

    handler_ref = weakref.ref(handler)
    other = machines()
    other.intercept("alu_mark", handler)
    del handler
    assert gone_model() is not None
    assert handler_ref() is not None
    kept.close()
    other.close()
    assert collected(gone_model)
    assert collected(handler_ref)


def test_close_takes_no_gil_entry_on_the_machine_thread(machines: Machines) -> None:
    m = machines()
    handle = m._handle  # pyright: ignore[reportPrivateUsage]
    before = handle.gil_entries()
    m.close()
    assert handle.gil_entries() == before


def test_close_from_a_second_thread_while_the_first_polls(machines: Machines) -> None:
    m = machines()
    pending = m.write((m.ref.reg.r0, 5), at=m.time + ms(10))
    m.intercept("alu_mark", lambda call: call.proceed())
    failure: list[BaseException] = []
    closer = threading.Thread(target=m.close)
    closer.start()
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        try:
            m.busy  # noqa: B018
        except popcorn.EmulationError:
            break
        except Exception as error:
            failure.append(error)
            break
    closer.join(10)
    assert not closer.is_alive()
    assert not failure, failure
    with pytest.raises(popcorn.EmulationError):
        asyncio.run(_await(pending))


async def _await(pending: Pending[object]) -> object:
    return await pending


def test_a_closed_machine_is_collected_without_a_finalizer_report(
    machines: Machines, monkeypatch: pytest.MonkeyPatch
) -> None:
    reported: list[str | None] = []

    def report(raised: sys.UnraisableHookArgs) -> None:
        reported.append(raised.err_msg)

    monkeypatch.setattr(sys, "unraisablehook", report)
    m = machines()
    gone = weakref.ref(m)
    m.close()
    del m
    assert collected(gone)
    assert reported == []


TIMER_CNT = 0x4003_000C


def test_a_lingering_view_keeps_the_host_but_the_model_is_refused_after_close() -> None:
    before = quiet_host_threads()
    Counted.reads = 0
    m = counted_machine()
    assert _core.host_threads() == before + 1
    gone = weakref.ref(m.model("TIMER"))
    table = m._callback_table  # pyright: ignore[reportPrivateUsage]
    view = m.mem
    assert view.u32[TIMER_CNT] == 7
    reads = Counted.reads
    m.close()
    assert table.closed
    assert view.u32[TIMER_CNT] == 0
    assert Counted.reads == reads
    assert _core.host_threads() == before + 1
    del view, table
    assert settled(lambda: _core.host_threads() == before)
    assert collected(gone)


class RaisingHalt:
    """A handle whose halt raises, as a late failure of an earlier command does."""

    def __init__(self, handle: object) -> None:
        self.handle = handle

    def halt(self) -> None:
        raise RuntimeError("late failure")

    def __getattr__(self, name: str) -> object:
        return getattr(self.handle, name)


def test_a_close_that_raises_still_leaves_a_closed_machine(machines: Machines) -> None:
    before = quiet_host_threads()
    m = machines()
    asyncio.run(m.run(budget=1_000))
    assert _core.host_threads() == before + 1
    spied: Any = m
    spied._handle = RaisingHalt(spied._handle)
    del spied
    with pytest.raises(RuntimeError, match="late failure"):
        m.close()
    with pytest.raises(popcorn.EmulationError, match="the machine was closed"):
        m.busy  # noqa: B018
    m.close()
    assert settled(lambda: _core.host_threads() == before)


def test_the_closed_stub_raises_for_every_protocol_use(machines: Machines) -> None:
    m = machines()
    m.close()
    stub: Any = m.reg

    async def awaited() -> None:
        await stub

    async def entered() -> None:
        async with stub:
            pass

    for use in (
        lambda: 1 in stub,
        lambda: next(stub),
        lambda: len(stub),
        lambda: bool(stub),
        lambda: hasattr(stub, "anything"),
        lambda: stub.anything,
        lambda: stub(),
        lambda: asyncio.run(awaited()),
        lambda: asyncio.run(entered()),
    ):
        with pytest.raises(popcorn.EmulationError, match="the machine was closed"):
            use()
    with pytest.raises(popcorn.EmulationError, match="the machine was closed"), stub:
        pass
    assert not hasattr(stub, "__wrapped__")
