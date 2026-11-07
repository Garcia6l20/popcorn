"""GPIO pins from a bus device: watch and drive through `DeviceBoard`, edge callbacks at their
stamps, on every GPIO row and on every device kind."""

from __future__ import annotations

import asyncio
import threading
import weakref
from collections.abc import Callable, Mapping, Sequence
from dataclasses import dataclass

import popcorn
import pytest
from popcorn import System, UartLink
from popcorn.arm import Reg
from popcorn.board import DeviceBoard, I2CTarget, PinMode, SpiTarget, UartDevice, Unwatch
from popcorn.units import VirtualDuration, VirtualTime, us

from .conftest import collected

BRANCH_TO_SELF = b"\xfe\xe7"
WFI_LOOP = b"\x30\xbf\xfd\xe7"
NOP = b"\x00\xbf"
TOGGLE = bytes.fromhex(
    "0b484ff020014ff40012016000bf00bf00bf00bf00bf00bf00bf00bf026000bf00bf00bf00bf00bf00bf00bf00bf"
    "ece7"
)
RAM = 0x2000_0000
THUMB = 1 << 24
BRR = 35
PS_PER_SECOND = 10**12

type Edges = list[tuple[bool, int]]


@dataclass(frozen=True)
class Row:
    part: str
    gpio_base: int
    clock: str


ROWS = {
    "u575": Row("STM32U575QGIx", 0x4202_0000, "AHB2ENR1"),
    "h573": Row("STM32H573AIIx", 0x4202_0000, "AHB2ENR"),
    "l152": Row("STM32L152RETx", 0x4002_0000, "AHBENR"),
    "l476": Row("STM32L476RGTx", 0x4800_0000, "AHB2ENR"),
}


@pytest.fixture(scope="session", params=sorted(ROWS))
def row(request: pytest.FixtureRequest) -> tuple[Row, popcorn.PartDescription]:
    from popcorn.stm32 import mcu
    from popcorn.stm32.description import DescriptionError

    chosen = ROWS[str(request.param)]
    try:
        return chosen, mcu(chosen.part)
    except DescriptionError as error:
        pytest.skip(str(error))


def nop_loop(nops: int) -> bytes:
    branch = 0xE000 | ((-(2 * nops + 4)) >> 1 & 0x7FF)
    return NOP * nops + branch.to_bytes(2, "little")


def toggle_for(chosen: Row) -> bytes:
    return TOGGLE + (chosen.gpio_base + 0x18).to_bytes(4, "little")


def stamp(loop: asyncio.AbstractEventLoop) -> int:
    return round(loop.time() * PS_PER_SECOND)


type Devices = Mapping[str, Sequence[UartDevice | I2CTarget | SpiTarget]]


def start(
    description: popcorn.PartDescription,
    devices: Devices,
    code: bytes = BRANCH_TO_SELF,
    *,
    clock: str = "AHB2ENR1",
    name: str | None = None,
) -> popcorn.Machine:
    m = popcorn.Machine(description, devices=devices, name=name)
    m.reset()
    getattr(m.periph.RCC, clock).GPIOAEN = 1
    set_mode(m, 5, 1)
    set_mode(m, 0, 0)
    set_mode(m, 1, 0)
    m.mem.write(RAM, code)
    m.reg[Reg.PC] = RAM
    m.reg[Reg.SP] = RAM + 0x800
    m.reg[Reg.XPSR] = THUMB
    m.reg[Reg.PRIMASK] = 1
    return m


def set_mode(m: popcorn.Machine, pin: int, mode: int) -> None:
    moder = m.periph.GPIOA.MODER
    field = f"MODE{pin}" if hasattr(moder, f"MODE{pin}") else f"MODER{pin}"
    setattr(moder, field, mode)


def run(m: popcorn.Machine, duration: VirtualDuration) -> None:
    asyncio.run(m.run(for_=duration))


def gil_entries(m: popcorn.Machine) -> int:
    return m._handle.gil_entries()  # pyright: ignore[reportPrivateUsage]


class Listener:
    """Records the edges of some pins, with the loop's stamp at each call."""

    def __init__(self, board: DeviceBoard, loop: Callable[[], asyncio.AbstractEventLoop]) -> None:
        self.board = board
        self.loop = loop
        self.edges: dict[str, Edges] = {}
        self.nows: list[int] = []
        self.watches: list[Unwatch] = []

    def watch(self, *pins: str) -> None:
        for pin in pins:
            self.edges.setdefault(pin, [])
            self.watches.append(self.board.watch_pin(pin, self._heard(pin)))

    def _heard(self, pin: str) -> Callable[[bool, VirtualTime], None]:
        def heard(level: bool, at: VirtualTime) -> None:
            self.edges[pin].append((level, at.ps))
            self.nows.append(stamp(self.loop()))

        return heard


class Plain(UartDevice):
    pass


def listening(device: UartDevice, *pins: str) -> Listener:
    listener = Listener(device.board, lambda: device.loop)
    listener.watch(*pins)
    return listener


def toggled(chosen: Row, description: popcorn.PartDescription) -> tuple[Edges, list[int], Edges]:
    device = Plain()
    m = start(description, {"USART1": [device]}, toggle_for(chosen), clock=chosen.clock)
    listener = listening(device, "PA5", "PA0")
    m.loop.call_later(20e-6, device.board.drive_pin, "PA0", True)
    run(m, us(40))
    assert device.board.pin_level("PA0")
    return listener.edges["PA5"], listener.nows, listener.edges["PA0"]


def test_a_firmware_toggle_reaches_on_edge_with_its_stamp_on_every_row(
    row: tuple[Row, popcorn.PartDescription],
) -> None:
    chosen, description = row
    edges, nows, driven = toggled(chosen, description)
    assert len(edges) >= 4
    assert [level for level, _ in edges] == [index % 2 == 0 for index in range(len(edges))]
    assert sorted(at for _, at in edges) == [at for _, at in edges]
    assert set(at for _, at in edges) <= set(nows)
    assert driven == [(True, us(20).ps)]
    assert toggled(chosen, description) == (edges, nows, driven)


def test_on_edge_stamps_equal_the_clock_a_memory_watch_reads_at_the_same_stores(
    u575: popcorn.PartDescription,
) -> None:
    chosen = ROWS["u575"]
    device = Plain()
    m = start(u575, {"USART1": [device]}, toggle_for(chosen))
    listener = listening(device, "PA5")
    seen: list[int] = []
    m.watch(
        chosen.gpio_base + 0x18,
        4,
        popcorn.WatchKind.WRITE,
        lambda hit: seen.append(m.time.ps),
    )
    run(m, us(40))
    stamps = [at for _, at in listener.edges["PA5"]]
    assert len(stamps) >= 4
    assert stamps == seen[: len(stamps)]


class Driver(UartDevice):
    def __init__(self, plan: Mapping[int, tuple[str, bool, VirtualDuration | None]]) -> None:
        super().__init__()
        self.plan = plan
        self.frames: list[tuple[int, int]] = []

    def on_frame(self, byte: int, at: VirtualTime) -> None:
        self.frames.append((byte, at.ps))
        pin, level, after = self.plan[byte]
        self.board.drive_pin(pin, level, at=None if after is None else at + after)


def drive_from_frames(
    description: popcorn.PartDescription, code: bytes
) -> tuple[Driver, Listener, popcorn.Machine]:
    driver = Driver({0x55: ("PA0", True, None), 0x56: ("PA1", True, us(5))})
    m = start(description, {"USART1": [driver]}, code)
    listener = listening(driver, "PA0", "PA1")
    m.periph.RCC.APB2ENR.USART1EN = 1
    m.periph.USART1.BRR.value = BRR
    m.periph.USART1.CR1_disabled.poke(UE=1, TE=1, RE=1)
    for byte in (0x55, 0x56):
        m.periph.USART1.TDR.value = byte
        run(m, us(600))
    return driver, listener, m


def test_drive_pin_from_on_frame_reaches_the_pin_at_the_stamp_at_any_block_shape(
    u575: popcorn.PartDescription,
) -> None:
    for code in (BRANCH_TO_SELF, nop_loop(40), nop_loop(37), WFI_LOOP):
        driver, listener, m = drive_from_frames(u575, code)
        (_, first), (_, second) = driver.frames
        assert listener.edges["PA0"] == [(True, first)]
        assert listener.edges["PA1"] == [(True, second + us(5).ps)]
        assert m.periph.GPIOA.pin(0).level
        assert m.periph.GPIOA.pin(1).level


@dataclass(frozen=True)
class Kind:
    peripheral: str
    make: Callable[[], UartDevice | I2CTarget | SpiTarget]


KINDS = {
    "uart": Kind("USART1", Plain),
    "i2c": Kind("I2C1", lambda: I2CTarget(0x50)),
    "spi": Kind("SPI1", lambda: SpiTarget(None)),
}


@pytest.fixture(params=sorted(KINDS))
def kind(request: pytest.FixtureRequest) -> Kind:
    return KINDS[str(request.param)]


def test_every_device_kind_watches_drives_and_reads_pins(
    u575: popcorn.PartDescription, kind: Kind
) -> None:
    device = kind.make()
    m = start(u575, {kind.peripheral: [device]})
    board = device.board
    listener = Listener(board, lambda: m.loop)
    listener.watch("PA0")
    assert board.pin_mode("PA5") is PinMode.OUTPUT
    assert board.pin_mode("PA0") is PinMode.INPUT
    assert not board.pin_level("PA0")
    levels: list[bool] = []

    def drive(level: bool, after: VirtualDuration | None) -> None:
        board.drive_pin(
            "PA0", level, at=None if after is None else VirtualTime(stamp(m.loop) + after.ps)
        )
        levels.append(board.pin_level("PA0"))

    m.loop.call_later(10e-6, drive, True, None)
    m.loop.call_later(20e-6, drive, False, us(3))
    run(m, us(40))
    assert board.pin_level("PA0") is False
    assert [level for level, _ in listener.edges["PA0"]] == [True, False]
    assert [at for _, at in listener.edges["PA0"]] == listener.nows
    assert [at for _, at in listener.edges["PA0"]] == [us(10).ps, us(23).ps]


def test_a_parked_call_drives_at_the_clock_and_a_past_stamp_is_refused(
    u575: popcorn.PartDescription,
) -> None:
    device = Plain()
    m = start(u575, {"USART1": [device]})
    listener = listening(device, "PA0")
    run(m, us(20))
    at = m.time
    device.loop.call_soon(device.board.drive_pin, "PA0", True)
    errors: list[BaseException] = []

    def past() -> None:
        try:
            device.board.drive_pin("PA0", False, at=VirtualTime(device.now.ps - 1))
        except ValueError as error:
            errors.append(error)

    device.loop.call_soon(past)
    run(m, us(5))
    assert listener.edges["PA0"] == [(True, at.ps)]
    assert len(errors) == 1 and "before now" in str(errors[0])
    with pytest.raises(RuntimeError, match="machine's thread"):
        device.board.drive_pin("PA0", False)


class Probing(UartDevice):
    def __init__(self) -> None:
        super().__init__()
        self.seen: list[tuple[PinMode, bool, bool]] = []
        self.pin = "PA5"

    def heard(self, level: bool, at: VirtualTime) -> None:
        self.seen.append((self.board.pin_mode(self.pin), self.board.pin_level(self.pin), self.rts))


def probed(
    description: popcorn.PartDescription, setup: Callable[[popcorn.Machine], None]
) -> Probing:
    device = Probing()
    m = start(description, {"USART1": [device]})
    device.board.watch_pin("PA0", device.heard)
    setup(m)
    m.periph.GPIOA.pin(0).level = True
    run(m, us(5))
    return device


def test_pin_mode_tells_a_driven_low_output_from_a_released_input(
    u575: popcorn.PartDescription,
) -> None:
    def driven_low(m: popcorn.Machine) -> None:
        m.periph.GPIOA.OTYPER.OT5 = 1
        m.periph.GPIOA.ODR.OD5 = 0

    def released(m: popcorn.Machine) -> None:
        set_mode(m, 5, 0)

    assert probed(u575, driven_low).seen == [(PinMode.OUTPUT, False, True)]
    assert probed(u575, released).seen == [(PinMode.INPUT, False, True)]


def test_an_alternate_function_pin_reads_its_rts_through_the_device(
    u575: popcorn.PartDescription,
) -> None:
    device = Probing()
    device.pin = "PA12"
    m = start(u575, {"USART1": [device]})
    m.periph.RCC.APB2ENR.USART1EN = 1
    m.periph.USART1.BRR.value = BRR
    m.periph.USART1.CR3.RTSE = 1
    m.periph.USART1.CR1_disabled.poke(UE=1, TE=1, RE=1)
    set_mode(m, 12, 2)
    m.periph.GPIOA.pin(12).level = True
    device.board.watch_pin("PA0", device.heard)
    device.loop.call_soon(device.send, b"A")
    run(m, us(300))
    m.periph.GPIOA.pin(0).level = True
    run(m, us(5))
    assert device.seen == [(PinMode.ALTERNATE, True, False)]


def test_refusals(u575: popcorn.PartDescription) -> None:
    device = Plain()
    board = device.board
    with pytest.raises(popcorn.ConfigError, match="serves no live machine"):
        board.pin_level("PA0")
    with pytest.raises(popcorn.ConfigError, match="serves no live machine"):
        board.watch_pin("PA0", lambda level, at: None)
    m = start(u575, {"USART1": [device]})
    for bad in ("", "A5", "PA16", "pa5", "PA", "PAA"):
        with pytest.raises(ValueError, match="named such as"):
            board.pin_level(bad)
    with pytest.raises(TypeError):
        board.pin_level(5)  # type: ignore[arg-type]
    with pytest.raises(popcorn.ConfigError, match="GPIOZ"):
        board.pin_level("PZ1")
    with pytest.raises(popcorn.ConfigError, match=r"bonded"):
        board.pin_level("PH15")
    with pytest.raises(TypeError):
        board.drive_pin("PA0", 1)  # type: ignore[arg-type]
    with pytest.raises(TypeError):
        board.drive_pin("PA0", True, at=5)  # type: ignore[arg-type]
    with pytest.raises(TypeError):
        board.watch_pin("PA0", 5)  # type: ignore[arg-type]
    assert m.periph.GPIOA.pin(0).level is False


def test_a_drive_from_another_thread_while_the_machine_runs_is_refused(
    u575: popcorn.PartDescription,
) -> None:
    device = Plain()
    m = start(u575, {"USART1": [device]}, nop_loop(40))
    errors: list[BaseException] = []

    async def body() -> None:
        async with m.running():

            def other() -> None:
                try:
                    device.board.drive_pin("PA0", True)
                except BaseException as error:
                    errors.append(error)

            thread = threading.Thread(target=other)
            thread.start()
            await asyncio.to_thread(thread.join)

    asyncio.run(body())
    assert len(errors) == 1
    assert isinstance(errors[0], RuntimeError | popcorn.MachineRunning)


def test_gil_entries_equal_the_edges_and_an_unwatched_pin_costs_none(
    u575: popcorn.PartDescription,
) -> None:
    chosen = ROWS["u575"]
    quiet = Plain()
    m = start(u575, {"USART1": [quiet]}, toggle_for(chosen))
    run(m, us(1))
    before = gil_entries(m)
    run(m, us(40))
    assert gil_entries(m) == before

    device = Plain()
    m = start(u575, {"USART1": [device]}, toggle_for(chosen))
    run(m, us(1))
    listener = listening(device, "PA5")
    base = gil_entries(m)
    run(m, us(40))
    assert len(listener.edges["PA5"]) >= 4
    assert gil_entries(m) - base == len(listener.edges["PA5"])


def test_a_pin_nobody_watches_posts_no_event(u575: popcorn.PartDescription) -> None:
    device = Plain()
    m = start(u575, {"USART1": [device]})
    pin = m.periph.GPIOA.pin(0)
    base = m.pending_events
    pin.level = True
    pin.level = False
    assert m.pending_events == base
    other = device.board.watch_pin("PA1", lambda level, at: None)
    pin.level = True
    assert m.pending_events == base
    watch = device.board.watch_pin("PA0", lambda level, at: None)
    pin.level = False
    assert m.pending_events == base + 1
    watch.remove()
    pin.level = True
    assert m.pending_events == base + 1
    other.remove()


def test_an_exception_from_on_edge_stops_the_run_as_a_model_error_naming_the_device(
    u575: popcorn.PartDescription,
) -> None:
    device = Plain()
    m = start(u575, {"USART1": [device]})

    def boom(level: bool, at: VirtualTime) -> None:
        raise LookupError("from the edge")

    held = device.board.watch_pin("PA0", boom)
    m.periph.GPIOA.pin(0).level = True
    with pytest.raises(popcorn.ModelError, match=r"Plain on USART1.*on_edge raised.*from the edge"):
        run(m, us(5))
    held.remove()
    run(m, us(5))


def test_a_removed_watch_hears_nothing_and_leaves_no_table_entry(
    u575: popcorn.PartDescription,
) -> None:
    device = Plain()
    m = start(u575, {"USART1": [device]})
    table = m._callback_table  # pyright: ignore[reportPrivateUsage]
    base = len(table)
    heard: list[bool] = []
    watch = device.board.watch_pin("PA0", lambda level, at: heard.append(level))
    assert watch.active and len(table) == base + 1
    asyncio.run(m.periph.GPIOA.pin(0).drive_edges([(VirtualTime(us(1).ps), True)]))
    watch.remove()
    watch.remove()
    assert not watch.active and len(table) == base
    run(m, us(5))
    assert heard == []
    for _ in range(1000):
        device.board.watch_pin("PA0", lambda level, at: None)()
    assert len(table) == base
    m.periph.GPIOA.pin(0).level = False
    m.periph.GPIOA.pin(0).level = True
    run(m, us(5))
    assert heard == []


class Captured:
    pass


def test_a_device_holding_its_watch_keeps_no_machine_alive(
    u575: popcorn.PartDescription,
) -> None:
    device = Plain()
    m = start(u575, {"USART1": [device]})
    seen: list[bool] = []
    captured = Captured()
    held = device.board.watch_pin("PA0", lambda level, at, captured=captured: seen.append(level))
    watched = weakref.ref(captured)
    del captured
    machine = weakref.ref(m)
    m.periph.GPIOA.pin(0).level = True
    run(m, us(5))
    assert seen == [True] and held.active
    del m
    assert collected(machine)
    assert collected(watched)
    assert not held.active
    held.remove()
    with pytest.raises(popcorn.ConfigError):
        device.board.pin_level("PA0")


def test_a_watch_stays_across_a_reset_and_a_reset_edge_is_silent(
    u575: popcorn.PartDescription,
) -> None:
    device = Plain()
    m = start(u575, {"USART1": [device]})
    listener = listening(device, "PA5")
    m.periph.GPIOA.ODR.OD5 = 1
    run(m, us(5))
    assert [level for level, _ in listener.edges["PA5"]] == [True]
    m.reset()
    assert not device.board.pin_level("PA5")
    run(m, us(5))
    assert [level for level, _ in listener.edges["PA5"]] == [True]
    m.periph.RCC.AHB2ENR1.GPIOAEN = 1
    set_mode(m, 5, 1)
    m.periph.GPIOA.ODR.OD5 = 1
    run(m, us(5))
    assert [level for level, _ in listener.edges["PA5"]] == [True, True]


SET_THEN_RESET = bytes.fromhex("034820210160034a034b1360fee70000180002420ced00e00400fa05")


def test_an_edge_pending_at_a_reset_request_is_dropped_with_the_queue(
    u575: popcorn.PartDescription,
) -> None:
    device = Plain()
    m = start(u575, {"USART1": [device]}, SET_THEN_RESET)
    listener = listening(device, "PA5")
    stop = asyncio.run(m.run(for_=us(20)))
    assert stop.reason is popcorn.StopReason.RESET_REQUEST
    assert m.periph.GPIOA.pin(5).level
    assert listener.edges["PA5"] == []
    asyncio.run(m.run(for_=us(20)))
    assert listener.edges["PA5"] == []
    assert not device.board.pin_level("PA5")


class BoardDevice(UartDevice):
    def __init__(self) -> None:
        super().__init__()
        self.edges: Edges = []
        self.watch: Unwatch | None = None

    def on_attach(self) -> None:
        self.watch = self.board.watch_pin("PA5", self.heard)

    def heard(self, level: bool, at: VirtualTime) -> None:
        self.edges.append((level, at.ps))


QUANTA = (20, 5, 1)


def toggling_member(
    description: popcorn.PartDescription, quantum_us: int
) -> tuple[System, BoardDevice, popcorn.Machine]:
    device = BoardDevice()
    chosen = ROWS["u575"]
    a = start(description, {"USART1": [device]}, toggle_for(chosen), name="a")
    b = start(description, {}, nop_loop(40), name="b")
    links = [
        UartLink("a.USART2", "b.USART2", latency=us(20), baud=114_285),
        UartLink("a.USART3", "b.USART3", latency=us(quantum_us), baud=114_285),
    ]
    return System({"a": a, "b": b}, links), device, a


def test_a_member_pin_watch_is_the_same_at_every_quantum_once_run_on(
    u575: popcorn.PartDescription,
) -> None:
    results: list[Edges] = []
    for quantum in QUANTA:
        system, device, _ = toggling_member(u575, quantum)
        with system:
            asyncio.run(system.run(for_=us(200)))
            asyncio.run(system.run(for_=us(200)))
        assert len(device.edges) >= 4
        assert device.edges == sorted(device.edges, key=lambda edge: edge[1])
        results.append(device.edges)
    assert results[0] == results[1] == results[2]


def test_a_member_parked_drive_stamps_at_the_clock_and_repeats_at_one_quantum(
    u575: popcorn.PartDescription,
) -> None:
    runs: list[Edges] = []
    for _ in range(2):
        system, device, a = toggling_member(u575, 5)
        listener = listening(device, "PA0")
        with system:
            asyncio.run(system.run(for_=us(200)))
            at = a.time.ps
            a.loop.call_soon(device.board.drive_pin, "PA0", True)
            asyncio.run(system.run(for_=us(50)))
        assert listener.edges["PA0"] == [(True, at)]
        runs.append(listener.edges["PA0"])
    assert runs[0] == runs[1]
