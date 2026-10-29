"""Shared inputs of the facade tests: the core test description and firmware."""

from __future__ import annotations

import asyncio
import contextlib
import gc
import os
import struct
import threading
import time
import weakref
from collections.abc import Callable, Coroutine, Iterator, Mapping, Sequence
from dataclasses import dataclass
from pathlib import Path

import popcorn
import pytest
from popcorn._refs import Pending
from popcorn.model import Model
from popcorn.units import VirtualDuration, VirtualTime, ms

from .linked import Build, links, member

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


def collected[T](watch: weakref.ref[T], rounds: int = 100) -> bool:
    """Whether the object of `watch` is gone after full collections, at most `rounds` of them.

    `gc.collect()` returns 0 at once while a collection runs on another thread, such as an
    automatic one on a pytest-timeout timer thread that finalizes an earlier machine. That
    machine's handle releases the GIL to halt its thread, so the collection is still running.
    Each round sleeps 10 ms, which lets that thread finish it.
    """
    for _ in range(rounds):
        gc.collect()
        if watch() is None:
            return True
        time.sleep(0.01)
    return False


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
def m33() -> popcorn.PartDescription:
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


class StorageModel(Model):
    """A model with no handler. Its peripheral keeps the described storage semantics and
    counts as modelled, so the host views reach it."""


class Machines:
    """Builds reset machines over the driver firmware, one scenario each."""

    def __init__(self, description: popcorn.PartDescription, image: popcorn.Image) -> None:
        self.description = description
        self.image = image

    def __call__(
        self,
        scenario: str | None = None,
        *,
        spin_threshold: int = 0,
        slice_instructions: int = 1_000_000,
        slice_wall_budget: float = 1e-3,
        models: Mapping[str, Callable[[], Model]] | None = None,
    ) -> popcorn.Machine:
        """A machine reset into `scenario`, the vector table `vt_<scenario>`, or the ALU loop."""
        table = None if scenario is None else self.image.lookup(f"vt_{scenario}").value
        m = popcorn.Machine(
            self.description,
            image=self.image,
            models=models,
            vector_table=table,
            spin_threshold=spin_threshold,
            slice_instructions=slice_instructions,
            slice_wall_budget=slice_wall_budget,
        )
        m.reset()
        return m


@pytest.fixture
def machines(m33: popcorn.PartDescription, driver: popcorn.Image) -> Machines:
    """Factory of reset machines over the driver firmware."""
    return Machines(m33, driver)


@pytest.fixture
def views(m33: popcorn.PartDescription) -> popcorn.Machine:
    """A machine over the views firmware, stopped at its bkpt after copying .data."""
    m = popcorn.Machine(m33, image=popcorn.Image.elf(VIEWS_ELF))
    m.reset()
    stop = asyncio.run(m.run(budget=100_000))
    assert stop.reason is popcorn.StopReason.USER
    return m


@pytest.fixture
def callee(m33: popcorn.PartDescription) -> popcorn.Machine:
    """A machine over the callee firmware, reset, not booted."""
    m = popcorn.Machine(m33, image=popcorn.Image.elf(CALLEE_ELF))
    m.reset()
    return m


@pytest.fixture
def hooked(m33: popcorn.PartDescription) -> popcorn.Machine:
    """A machine over the hooks firmware, stopped at its bkpt after copying .data."""
    m = popcorn.Machine(m33, image=popcorn.Image.elf(HOOKS_ELF))
    m.reset()
    assert asyncio.run(m.run(budget=100_000)).reason is popcorn.StopReason.USER
    return m


BOARD_ELF = ROOT / "tests" / "firmware" / "api" / "prebuilt" / "board.elf"


@pytest.fixture(scope="session")
def u575() -> popcorn.PartDescription:
    """The STM32U575 description built from ST's files, the test skipped without them."""
    from popcorn.stm32 import mcu
    from popcorn.stm32.description import DescriptionError

    try:
        return mcu("STM32U575QGIx")
    except DescriptionError as error:
        pytest.skip(f"no STM32U575 description: {error}")


PERIPH_FIRMWARE = ROOT / "tests" / "firmware" / "periph" / "prebuilt"
API_FIRMWARE = ROOT / "tests" / "firmware" / "api" / "prebuilt"


@dataclass(frozen=True, slots=True)
class TzPart:
    """A part the TrustZone cases run on, with its test firmware.

    `rcc_elf` is the RCC LL firmware of the part, with an `idle` loop. `pair` and `pair_s` are
    its Non-secure and Secure test pair: the Non-secure ELF embeds the Secure binary at
    0x08000000, the Secure ELF gives its symbols, both built for that part. `mpcbb3_words` is
    the number of SECCFGR words of MPCBB3, one per 16-Kbyte super-block of SRAM3.
    `flash_block` names the FLASH register of the Secure bits of bank 1 sectors 0 to 31, and
    `system_config` the unit whose SECCFGR the model guards, SYSCFG on the U575 and SBS on the
    H573.
    """

    description: popcorn.PartDescription
    rcc_elf: Path
    pair: Path
    pair_s: Path
    mpcbb3_words: int
    flash_block: str
    system_config: str

    @property
    def sram_size(self) -> int:
        """Size of the contiguous SRAM region at 0x20000000."""
        return next(r.size for r in self.description.regions if r.name == "SRAM_NS")

    def address(self, peripheral: str, register: str) -> int:
        """Bus address of `register` of `peripheral`."""
        unit = next(p for p in self.description.peripherals if p.name == peripheral)
        return unit.base + unit.register(register).offset

    def bit(self, peripheral: str, register: str, field: str) -> int:
        """The lowest bit of `field` of `register` of `peripheral`, as a mask."""
        unit = next(p for p in self.description.peripherals if p.name == peripheral)
        return 1 << next(f.lsb for f in unit.register(register).fields if f.name == field)


TZ_PARTS = {
    "u575": ("STM32U575QGIx", PERIPH_FIRMWARE, API_FIRMWARE, 32, "SEC1BBR1", "SYSCFG"),
    "h573": (
        "STM32H573AIIx",
        PERIPH_FIRMWARE / "h573",
        PERIPH_FIRMWARE / "h573",
        20,
        "SECBB1R1",
        "SBS",
    ),
}
"""Part, RCC firmware directory, test pair directory, MPCBB3 SECCFGR count, FLASH block register
of bank 1 sectors 0 to 31 and guarded system configuration unit of each TrustZone part, RM0456
Table 33, 7.9.29 and 15.3, RM0481 Table 33, 7.11.31 and 14.5.8."""


def tz_part(key: str) -> TzPart:
    """The TrustZone part `key` of `TZ_PARTS`, the test skipped without its description."""
    from popcorn.stm32 import mcu
    from popcorn.stm32.description import DescriptionError

    part, rcc, pair, words, flash_block, system_config = TZ_PARTS[key]
    try:
        description = mcu(part)
    except DescriptionError as error:
        pytest.skip(f"no {part} description: {error}")
    return TzPart(
        description,
        rcc / "rcc_ll.elf",
        pair / "tz_pair.elf",
        pair / "tz_pair_s.elf",
        words,
        flash_block,
        system_config,
    )


@pytest.fixture(scope="session", params=sorted(TZ_PARTS))
def tz(request: pytest.FixtureRequest) -> TzPart:
    """Each TrustZone part in turn."""
    return tz_part(str(request.param))


@pytest.fixture(scope="session")
def tz_u575() -> TzPart:
    """The STM32U575, for the TrustZone cases that name registers only the STM32U5 has."""
    return tz_part("u575")


@pytest.fixture
def board(u575: popcorn.PartDescription) -> popcorn.Machine:
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


@dataclass
class Stall:
    """A sample that holds the machine's thread at one boundary until `release` is set."""

    reached: threading.Event
    release: threading.Event
    boundary: list[VirtualTime]
    holding: Pending[None]


def stall(m: popcorn.Machine, at: VirtualTime | None = None) -> Stall:
    """Post a sample that holds the machine's thread at the first boundary at or after `at`.
    Posted while the machine is parked, with `at` after `m.time`, it waits for the free run, so
    the hold comes at a boundary fixed in virtual time however loaded the host is. Posted
    inside `running()`, the hold comes wherever the free run has got to."""
    reached = threading.Event()
    release = threading.Event()
    boundary: list[VirtualTime] = []

    def held() -> None:
        boundary.append(m.time)
        reached.set()
        release.wait(10.0)

    return Stall(reached, release, boundary, m.sample(held, at=at))


async def stalled[T](
    m: popcorn.Machine,
    post: Callable[[VirtualTime], Coroutine[object, object, T]],
    at: VirtualTime | None = None,
    held: Stall | None = None,
) -> tuple[VirtualTime, T]:
    """Inside `running()`, hold the machine's thread in a sample, start `post(boundary)` with
    the sample's boundary time, and release the machine once `post` has run ten loop turns.
    What it posts registers at that boundary, and times it derives from the boundary hold
    however loaded the host is. Return the boundary and the result of `post`.

    `at` stamps the sample: the hold comes at the first boundary at or after it. `held` is a
    `stall` posted before `running()`, used in place of a new sample."""
    if held is None:
        held = stall(m, at)
    while not held.reached.is_set():
        await asyncio.sleep(0.001)
    task = asyncio.create_task(post(held.boundary[0]))
    for _ in range(10):
        await asyncio.sleep(0)
    held.release.set()
    await held.holding
    return held.boundary[0], await task


async def stalled_until_posted[T](
    m: popcorn.Machine, post: Callable[[], Coroutine[object, object, T]]
) -> tuple[VirtualTime, T]:
    """As `stalled`, releasing the machine once `post()` has run ten loop turns rather than
    after 50 ms of wall time, so the post registers at the boundary however loaded the host is."""
    release = threading.Event()

    def held() -> VirtualTime:
        release.wait(10.0)
        return m.time

    holding = m.sample(held)
    task = asyncio.create_task(post())
    for _ in range(10):
        await asyncio.sleep(0)
    release.set()
    return await holding, await task


async def late_drained_read[T](
    m: popcorn.Machine,
    post: Callable[[VirtualTime], None],
    read: Callable[[VirtualTime], Coroutine[object, object, T]],
    past: VirtualDuration,
) -> tuple[VirtualTime, T]:
    """Inside `running()`, hold the machine's thread at a boundary, call `post(boundary)` and
    start `read(boundary)` there, then block the event loop until a sample stamped at
    `boundary + past` ran, so the free run is past that time before the read resumes."""
    reached = threading.Event()
    release = threading.Event()
    passed = threading.Event()
    boundary: list[VirtualTime] = []

    def held() -> None:
        boundary.append(m.time)
        reached.set()
        release.wait(10.0)

    holding = m.sample(held)
    while not reached.is_set():
        await asyncio.sleep(0.001)
    try:
        post(boundary[0])
        stamped = m.sample(passed.set, at=boundary[0] + past)
        task = asyncio.create_task(read(boundary[0]))
        for _ in range(10):
            await asyncio.sleep(0)
    finally:
        release.set()
    passed.wait(10.0)
    await stamped
    await holding
    return boundary[0], await task


LOOP_CODE = 0x2008_0000
LOAD_EACH = (0xF850, 0x2B04, 0x7813, 0x3901, 0xD1FA, 0xBE01)


def load_each(m: popcorn.Machine, addresses: Sequence[int]) -> int:
    """Put a firmware loop in SRAM that loads one byte from each of `addresses` and stops at its
    bkpt, and point the core at it. A firmware access to an offset without register logs one
    record, where a host access raises ModelError. Returns the instructions the loop runs."""
    table = LOOP_CODE + 0x10
    m.mem.write(LOOP_CODE, struct.pack(f"<{len(LOAD_EACH)}H", *LOAD_EACH))
    m.mem.write(table, struct.pack(f"<{len(addresses)}I", *addresses))
    m.reg.r0 = table
    m.reg.r1 = len(addresses)
    m.reg.pc = LOOP_CODE
    return 4 * len(addresses) + 1


def load_each_unforwarded(m: popcorn.Machine, addresses: Sequence[int]) -> None:
    """Run `load_each` in a free run no event loop awaits, so every record it logs stays queued
    until the `halt` that ends it flushes them, as a parked access's records do."""
    load_each(m, addresses)
    ended = threading.Event()

    def stop(call: popcorn.Call) -> popcorn.Return:
        ended.set()
        return call.stop()

    with m.intercepted(LOOP_CODE + 2 * (len(LOAD_EACH) - 1), stop):
        m.start()
        assert ended.wait(10)
        with contextlib.suppress(popcorn.EmulationError):
            m.halt()


@pytest.fixture
def pair(u575: popcorn.PartDescription) -> Iterator[Build]:
    """Builds `a` running `a_entry` linked USART1 to USART1 with `b` running `b_entry`."""
    made: list[popcorn.System] = []

    def build(
        slice_instructions: int = 1_000_000,
        third: VirtualDuration | None = None,
        *,
        a_entry: str = "link_tx",
        b_entry: str = "link_rx",
        baud: int = 460_800,
    ) -> popcorn.System:
        a = member(u575, a_entry, "a", slice_instructions)
        b = member(u575, b_entry, "b", slice_instructions)
        system = popcorn.System(
            {"a": a, "b": b}, links(third, baud), slice_instructions=slice_instructions
        )
        made.append(system)
        return system

    yield build
    for one in made:
        one.close()
