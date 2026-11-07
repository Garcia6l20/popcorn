"""A lent `machine` fixture and its virtual-time loop: what test A leaves never reaches test B,
what its `finally` raises lands on test A, and a stack built at boot survives each restore."""

from __future__ import annotations

from pathlib import Path

import pytest

from .helpers import BOARD_ELF, write_conftest, write_project_conftest

pytest_plugins = ["pytester"]

BASE = ("-p", "no:asyncio")

DEVICE = """
import asyncio
import contextvars
from popcorn.board import UartDevice
from popcorn.units import VirtualTime


VALUE = contextvars.ContextVar("value", default="none")


class Echoed(UartDevice):
    def __init__(self):
        super().__init__()
        self.frames = []
        self.values = []
        self.builds = 0
        self.fail_restore = False

    def on_attach(self):
        self.builds += 1

    def on_frame(self, byte, at):
        self.frames.append((hex(byte), at.ps))
        self.values.append(VALUE.get())
        VALUE.set("set")


class TimedAtBoot(Echoed):
    def __init__(self):
        super().__init__()
        self.fired = []

    def on_attach(self):
        super().on_attach()
        self.loop.call_later(1e-5, self.fired.append, "boot timer")


class Talking(Echoed):
    def on_attach(self):
        self.send(b"READY")


class Overriding(Echoed):
    def __init__(self):
        super().__init__()
        self.edges = []
        self.fail_restore = False

    def on_attach(self):
        super().on_attach()
        self.board.watch_pin("PA0", lambda level, at: self.edges.append(level))

    def snapshot(self):
        return b""

    def restore(self, data):
        if self.fail_restore:
            self.fail_restore = False
            raise LookupError("from restore")
"""


@pytest.fixture(scope="module")
def u575_json() -> Path:
    from popcorn.stm32.description import DescriptionError, cached_description

    try:
        return Path(cached_description("STM32U575QGIx"))
    except DescriptionError as error:
        pytest.skip(f"no STM32U575 description: {error}")


def project(pytester: pytest.Pytester, u575_json: Path, device: str = "Echoed") -> None:
    write_conftest(pytester)
    write_project_conftest(
        pytester.path,
        "popcorn.MachineDescription(\n"
        '    part="STM32U575QGIx",\n'
        f"    description=Path({str(u575_json)!r}),\n"
        f"    images=[popcorn.Elf(Path({str(BOARD_ELF)!r}))],\n"
        '    boot=popcorn.Boot(until="ready", budget=1_000_000),\n'
        f'    board={{"USART1": popcorn.Periph(devices=[popcorn.Device({device})])}},\n'
        ")",
        DEVICE,
    )


def run(pytester: pytest.Pytester, *args: str) -> pytest.RunResult:
    return pytester.runpytest(*BASE, "-p", "no:cacheprovider", *args)


def test_a_raising_timer_of_test_a_stays_out_of_test_b(
    pytester: pytest.Pytester, u575_json: Path
) -> None:
    project(pytester, u575_json)
    pytester.makepyfile("""
        from popcorn.units import ms

        def boom():
            raise LookupError("from test a")

        def test_a(machine):
            machine.loop.call_later(1e-4, boom)

        async def test_b(machine):
            await machine.run(for_=ms(1))
        """)
    run(pytester).assert_outcomes(passed=2)


def test_a_timer_that_sends_in_test_a_puts_no_byte_in_test_b(
    pytester: pytest.Pytester, u575_json: Path
) -> None:
    project(pytester, u575_json)
    pytester.makepyfile("""
        from popcorn.units import ms

        def test_a(machine):
            device = machine.board.devices("USART1")[0]
            device.loop.call_later(1e-4, device.send, b"Z")

        async def test_b(machine):
            device = machine.board.devices("USART1")[0]
            device.frames.clear()
            await machine.run(for_=ms(2))
            assert device.frames == []
            assert machine.periph.USART1.ISR_enabled.RXFNE == 0
        """)
    run(pytester).assert_outcomes(passed=2)


def test_a_sleeping_task_of_test_a_never_wakes_in_test_b(
    pytester: pytest.Pytester, u575_json: Path
) -> None:
    project(pytester, u575_json)
    pytester.makepyfile("""
        import asyncio
        from popcorn.units import ms

        WOKE = []

        async def sleeper():
            await asyncio.sleep(1e-4)
            WOKE.append(True)

        def test_a(machine):
            machine.loop.create_task(sleeper())

        async def test_b(machine):
            await machine.run(for_=ms(1))
            assert WOKE == []
        """)
    run(pytester).assert_outcomes(passed=2)


def test_a_far_timer_of_test_a_leaves_nothing_armed_in_test_b(
    pytester: pytest.Pytester, u575_json: Path
) -> None:
    project(pytester, u575_json)
    pytester.makepyfile("""
        from popcorn.units import us

        def test_a(machine):
            machine.loop.call_later(3600, print)
            assert machine.loop._armed is not None

        async def test_b(machine):
            await machine.run(for_=us(1))
            assert machine.loop._armed is None
        """)
    run(pytester).assert_outcomes(passed=2)


def test_a_finally_runs_in_the_teardown_of_test_a_and_its_error_lands_there(
    pytester: pytest.Pytester, u575_json: Path
) -> None:
    project(pytester, u575_json)
    pytester.makepyfile("""
        import asyncio

        SEEN = []

        async def pending():
            try:
                await asyncio.sleep(3600)
            finally:
                await asyncio.sleep(0)
                SEEN.append("finally")
                raise LookupError("from the finally of test a")

        async def test_a(machine):
            machine.loop.create_task(pending())
            from popcorn.units import us
            await machine.run(for_=us(1))
            assert SEEN == []

        def test_b(machine):
            assert SEEN == ["finally"]
        """)
    result = run(pytester)
    result.assert_outcomes(passed=2, errors=1)
    result.stdout.fnmatch_lines(["*ERROR at teardown of test_a*", "*from the finally of test a*"])


def test_a_stack_built_at_boot_survives_each_restore(
    pytester: pytest.Pytester, u575_json: Path
) -> None:
    project(pytester, u575_json, "TimedAtBoot")
    pytester.makepyfile("""
        from popcorn.units import ms

        async def hears_the_rebuilt_timer(machine):
            device = machine.board.devices("USART1")[0]
            device.fired.clear()
            await machine.run(for_=ms(1))
            assert device.fired == ["boot timer"]

        async def test_a(machine):
            await hears_the_rebuilt_timer(machine)

        async def test_b(machine):
            await hears_the_rebuilt_timer(machine)
        """)
    run(pytester).assert_outcomes(passed=2)


def test_a_talking_boot_errors_every_test_naming_the_device(
    pytester: pytest.Pytester, u575_json: Path
) -> None:
    project(pytester, u575_json, "Talking")
    pytester.makepyfile("""
        def test_a(machine):
            pass

        def test_b(machine):
            pass
        """)
    result = run(pytester)
    result.assert_outcomes(errors=2)
    result.stdout.fnmatch_lines(["*SnapshotRefused*Talking on USART1*override snapshot*"])


def test_a_model_error_at_a_lent_restore_raises_on_the_test(
    pytester: pytest.Pytester, u575_json: Path
) -> None:
    project(pytester, u575_json, "Overriding")
    pytester.makepyfile("""
        SEEN = []

        def test_a(machine):
            SEEN.append(machine)
            machine.board.devices("USART1")[0].fail_restore = True

        def test_b(machine):
            raise AssertionError("the restore raises ahead of the test")

        def test_c(machine):
            assert machine is SEEN[0]
        """)
    result = run(pytester)
    result.assert_outcomes(passed=2, errors=1)
    result.stdout.fnmatch_lines(["*ERROR at setup of test_b*", "*from restore*"])


def test_a_watch_of_test_a_ends_and_one_built_at_boot_stays(
    pytester: pytest.Pytester, u575_json: Path
) -> None:
    project(pytester, u575_json, "Overriding")
    pytester.makepyfile("""
        from popcorn.units import us

        A = []

        async def test_a(machine):
            device = machine.board.devices("USART1")[0]
            device.board.watch_pin("PA0", lambda level, at: A.append(level))

        async def test_b(machine):
            device = machine.board.devices("USART1")[0]
            device.edges.clear()
            machine.periph.RCC.AHB2ENR1.GPIOAEN = 1
            machine.periph.GPIOA.pin(0).level = True
            await machine.run(for_=us(5))
            assert A == []
            assert device.edges == [True]
        """)
    run(pytester).assert_outcomes(passed=2)


def test_a_contextvar_of_test_a_stays_out_of_test_b(
    pytester: pytest.Pytester, u575_json: Path
) -> None:
    project(pytester, u575_json)
    pytester.makepyfile("""
        from popcorn.units import ms

        async def heard_with(machine):
            device = machine.board.devices("USART1")[0]
            device.values.clear()
            device.loop.call_later(1e-5, device.send, b"Z")
            await machine.run(for_=ms(2))
            return list(device.values)

        async def test_a(machine):
            assert (await heard_with(machine))[0] == "none"
            assert (await heard_with(machine))[0] == "set"

        async def test_b(machine):
            assert (await heard_with(machine))[0] == "none"
        """)
    run(pytester).assert_outcomes(passed=2)
