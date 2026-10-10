"""Shared inputs of the facade tests: the core test description and firmware."""

from __future__ import annotations

import asyncio
import contextlib
import os
import threading
from collections.abc import Callable, Coroutine
from pathlib import Path

import popcorn
import pytest
from popcorn.units import VirtualTime, ms

ROOT = Path(__file__).parents[3]
M33_JSON = ROOT / "tests" / "data" / "core" / "m33.json"
CORE_FIRMWARE = ROOT / "tests" / "firmware" / "core" / "prebuilt"
DRIVER_ELF = ROOT / "tests" / "firmware" / "api" / "prebuilt" / "driver.elf"
HOOKS_ELF = ROOT / "tests" / "firmware" / "api" / "prebuilt" / "hooks.elf"
ABI_ELF = ROOT / "tests" / "firmware" / "abi" / "prebuilt" / "abi.elf"
CALLEE_ELF = ROOT / "tests" / "firmware" / "api" / "prebuilt" / "callee.elf"
VIEWS_ELF = ROOT / "tests" / "firmware" / "api" / "prebuilt" / "views.elf"
ISR_ELF = ROOT / "tests" / "firmware" / "api" / "prebuilt" / "isr.elf"


def run_bounded[T](coroutine: Coroutine[object, object, T], seconds: float = 2) -> T:
    """Run `coroutine` like `asyncio.run`, failing the test instead of hanging past `seconds`."""

    async def bounded() -> T:
        task = asyncio.create_task(coroutine)
        done, _ = await asyncio.wait([task], timeout=seconds)
        if not done:
            task.cancel()
            with contextlib.suppress(BaseException):
                await task
            pytest.fail(f"still waiting after {seconds} s of wall time")
        return task.result()

    return asyncio.run(bounded())


def holds_pty(path: str) -> bool:
    """Whether this process still has either end of pty `path` open.

    Not `os.path.exists(path)`: devpts frees the index at the last close, and the next
    `openpty()` of any process, a parallel test worker included, takes it again. The master
    end links to `/dev/ptmx`, its index in the `tty-index` line of its fdinfo. The slave end
    links to `<path> (deleted)` once its master is closed.
    """
    index = f"tty-index:\t{path.removeprefix('/dev/pts/')}"
    for fd in os.listdir("/proc/self/fd"):
        try:
            target = os.readlink(f"/proc/self/fd/{fd}")
            if target.removesuffix(" (deleted)") == path or (
                target in ("/dev/ptmx", "/dev/pts/ptmx")
                and index in Path(f"/proc/self/fdinfo/{fd}").read_text().splitlines()
            ):
                return True
        except OSError:
            continue
    return False


@pytest.fixture
def m33_json() -> Path:
    """Path of the core test description."""
    return M33_JSON


@pytest.fixture
def core_firmware() -> Path:
    """Directory of the prebuilt core test firmware."""
    return CORE_FIRMWARE


@pytest.fixture
def m33() -> popcorn.MachineDescription:
    """The core test description, a Cortex-M33 with flash, SRAM and test peripherals."""
    return popcorn.load_description(M33_JSON)


@pytest.fixture
def basic() -> popcorn.Image:
    """The basic core test firmware."""
    return popcorn.Image.elf(CORE_FIRMWARE / "basic.elf")


@pytest.fixture
def driver() -> popcorn.Image:
    """The driver test firmware: an ALU loop from reset, other scenarios behind `vt_*` tables."""
    return popcorn.Image.elf(DRIVER_ELF)


class Machines:
    """Builds reset machines over the driver firmware, one scenario each."""

    def __init__(self, description: popcorn.MachineDescription, image: popcorn.Image) -> None:
        self.description = description
        self.image = image

    def __call__(
        self,
        scenario: str | None = None,
        *,
        spin_threshold: int = 0,
        slice_instructions: int = 1_000_000,
        slice_wall_budget: float = 1e-3,
    ) -> popcorn.Machine:
        """A machine reset into `scenario`, the vector table `vt_<scenario>`, or the ALU loop."""
        table = None if scenario is None else self.image.lookup(f"vt_{scenario}").value
        m = popcorn.Machine(
            self.description,
            image=self.image,
            vector_table=table,
            spin_threshold=spin_threshold,
            slice_instructions=slice_instructions,
            slice_wall_budget=slice_wall_budget,
        )
        m.reset()
        return m


@pytest.fixture
def machines(m33: popcorn.MachineDescription, driver: popcorn.Image) -> Machines:
    """Factory of reset machines over the driver firmware."""
    return Machines(m33, driver)


@pytest.fixture
def views(m33: popcorn.MachineDescription) -> popcorn.Machine:
    """A machine over the views firmware, stopped at its bkpt after copying .data."""
    m = popcorn.Machine(m33, image=popcorn.Image.elf(VIEWS_ELF))
    m.reset()
    stop = asyncio.run(m.run(budget=100_000))
    assert stop.reason is popcorn.StopReason.USER
    return m


@pytest.fixture
def callee(m33: popcorn.MachineDescription) -> popcorn.Machine:
    """A machine over the callee firmware, reset, not booted."""
    m = popcorn.Machine(m33, image=popcorn.Image.elf(CALLEE_ELF))
    m.reset()
    return m


@pytest.fixture
def hooked(m33: popcorn.MachineDescription) -> popcorn.Machine:
    """A machine over the hooks firmware, stopped at its bkpt after copying .data."""
    m = popcorn.Machine(m33, image=popcorn.Image.elf(HOOKS_ELF))
    m.reset()
    assert asyncio.run(m.run(budget=100_000)).reason is popcorn.StopReason.USER
    return m


BOARD_ELF = ROOT / "tests" / "firmware" / "api" / "prebuilt" / "board.elf"


@pytest.fixture(scope="session")
def u575() -> popcorn.MachineDescription:
    """The STM32U575 description built from ST's files, the test skipped without them."""
    from popcorn.stm32 import mcu
    from popcorn.stm32.description import DescriptionError

    try:
        return mcu("STM32U575QGIx")
    except DescriptionError as error:
        pytest.skip(f"no STM32U575 description: {error}")


@pytest.fixture
def board(u575: popcorn.MachineDescription) -> popcorn.Machine:
    """A U575 machine over the board firmware, at `ready`: USART1 echoes, PA5 follows PA0."""
    m = popcorn.Machine(u575, image=popcorn.Image.elf(BOARD_ELF))
    m.reset()
    stop = asyncio.run(m.run(until=m.sym("ready"), budget=1_000_000))
    assert stop.reason is popcorn.StopReason.ADDRESS
    return m


REFUSED_INSIDE: dict[type[Exception], str] = {
    RuntimeError: "a handler accesses the machine directly, it cannot await a machine request",
    popcorn.MachineBusy: "a run from inside a callback of this machine",
}
"""The message of each error a request from inside a sampled callable raises."""


def requests_inside(
    m: popcorn.Machine,
) -> dict[str, tuple[Callable[[], object], type[Exception]]]:
    """Requests a sampled callable must not make, each as a call, with the error it raises."""
    return {
        "read": (lambda: m.read(), RuntimeError),
        "sample": (lambda: m.sample(lambda: None), RuntimeError),
        "wait_until": (lambda: m.wait_until(lambda _: True).send(None), RuntimeError),
        "run": (lambda: m.run(for_=ms(1)).send(None), popcorn.MachineBusy),
        "step": (lambda: m.step().send(None), popcorn.MachineBusy),
        "wait_for": (
            lambda: m.wait_for(popcorn.Reached(0), for_=ms(1)).send(None),
            popcorn.MachineBusy,
        ),
    }


REQUESTS_INSIDE = ("read", "sample", "wait_until", "run", "step", "wait_for")


async def stalled[T](
    m: popcorn.Machine, post: Callable[[], Coroutine[object, object, T]]
) -> tuple[VirtualTime, T]:
    """Inside `running()`, hold the machine's thread in a sample while `post()` starts, so what
    it posts registers at the sample's boundary. Return that boundary's time and its result."""
    release = threading.Event()

    def held() -> VirtualTime:
        release.wait(10.0)
        return m.time

    holding = m.sample(held)
    task = asyncio.create_task(post())
    await asyncio.sleep(0.05)
    release.set()
    return await holding, await task
