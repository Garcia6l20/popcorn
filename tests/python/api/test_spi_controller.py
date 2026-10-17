"""popcorn.board.SpiController: a host controller clocking an SPI the firmware runs in target
mode."""

from __future__ import annotations

import asyncio
import gc
import struct
import weakref
from collections.abc import Sequence
from pathlib import Path
from typing import cast, overload

import popcorn
import pytest
from popcorn import ConfigError, StopReason
from popcorn.board import SpiController, SpiTarget
from popcorn.pytest._cache import devices_digest
from popcorn.units import VirtualDuration, VirtualTime, ms, us

from .conftest import run_bounded, stall, stalled
from .test_host_binding import COLLECTED

SPI_ELF = Path(__file__).parents[2] / "firmware" / "periph" / "prebuilt" / "spi_ll.elf"
BUDGET = 100_000_000
START = ms(1)
FRAME_PS = 8_000_000
ECHO_LOG = [1, 2, 3, 4, 0]
ECHOED = [0xA0, 0xA1, 0x11, 0x12]


class Stamped(SpiController):
    def __init__(self, *, sck_hz: int = 1_000_000, bits: int = 8, mode: int | None = None) -> None:
        super().__init__(sck_hz=sck_hz, bits=bits, mode=mode)
        self.frames: list[tuple[int, int, int]] = []

    def on_frame(self, sent: int, received: int, at: VirtualTime) -> None:
        super().on_frame(sent, received, at)
        self.frames.append((sent, received, at.ps))


def log(m: popcorn.Machine) -> list[int]:
    count = m.mem.u32[m.sym("log_len")]
    raw = m.mem.read(m.sym("log_buf"), 4 * min(count, 256))
    return list(struct.unpack(f"<{len(raw) // 4}I", raw))


def echo_machine(
    u575: popcorn.PartDescription, controller: SpiController | None = None
) -> tuple[popcorn.Machine, SpiController]:
    device = Stamped() if controller is None else controller
    m = popcorn.Machine(u575, image=popcorn.Image.elf(SPI_ELF), devices={"SPI1": [device]})
    m.reset()
    stop = asyncio.run(m.run(until="main", budget=BUDGET))
    assert stop.reason is StopReason.ADDRESS
    m.reg.pc = m.sym("target_echo")
    return m, device


def idle_machine(u575: popcorn.PartDescription, device: SpiController) -> popcorn.Machine:
    m, _ = echo_machine(u575, device)
    m.reg.pc = m.sym("idle")
    return m


def finish(m: popcorn.Machine) -> list[int]:
    stop = asyncio.run(m.run(budget=BUDGET))
    assert stop.reason is StopReason.USER, stop
    return log(m)


def gil_entries(m: popcorn.Machine) -> int:
    return m._handle.gil_entries()  # pyright: ignore[reportPrivateUsage]


def check_frames(device: Stamped, at: VirtualTime) -> None:
    assert device.frames == [
        (index + 1, echoed, at.ps + (index + 1) * FRAME_PS) for index, echoed in enumerate(ECHOED)
    ]


def test_a_controller_clocks_the_firmware_target_and_hears_its_frames(
    u575: popcorn.PartDescription,
) -> None:
    m, device = echo_machine(u575)
    at = m.time + START
    events = m.pending_events
    assert asyncio.run(device.transfer([1, 2, 3, 4], at=at)) == at
    assert m.pending_events == events + 1
    assert finish(m) == ECHO_LOG
    assert device.received == ECHOED
    check_frames(cast(Stamped, device), at)
    assert m.pending_events == events


def test_a_transfer_is_alike_parked_and_running(u575: popcorn.PartDescription) -> None:
    m, parked = echo_machine(u575)
    at = m.time + START
    asyncio.run(parked.transfer([1, 2, 3, 4], at=at))
    parked_log = finish(m)
    m, running = echo_machine(u575)
    assert m.time + START == at
    held = stall(m, m.time + us(1))

    async def post(boundary: VirtualTime) -> VirtualTime:
        assert boundary < at
        return await running.transfer([1, 2, 3, 4], at=at)

    async def body() -> None:
        async with m.running():
            _, start = await stalled(m, post, held=held)
            assert start == at
            await m.wait_for(m.ref.var("log_len").ge(5))

    with pytest.raises(popcorn.EmulationError, match=r"USER at .* target_echo"):
        run_bounded(body(), 30)
    assert log(m) == parked_log == ECHO_LOG
    assert cast(Stamped, running).frames == cast(Stamped, parked).frames
    check_frames(cast(Stamped, running), at)


def test_each_frame_takes_the_gil_once_and_a_run_without_frames_none(
    u575: popcorn.PartDescription,
) -> None:
    m, device = echo_machine(u575)
    before = gil_entries(m)
    asyncio.run(m.run(for_=START))
    assert gil_entries(m) == before
    asyncio.run(device.transfer([1, 2, 3, 4]))
    assert finish(m) == ECHO_LOG
    assert gil_entries(m) - before == 4


def test_a_controller_alone_and_unselected_hears_an_idle_miso(
    u575: popcorn.PartDescription,
) -> None:
    device = SpiController(sck_hz=2_000_000, bits=16)
    m = idle_machine(u575, device)
    assert asyncio.run(device.transfer([0xFFFF, 0])) == m.time
    asyncio.run(m.run(for_=us(20)))
    assert device.received == [0xFFFF, 0xFFFF]


@pytest.mark.parametrize("queued", [False, True], ids=["completed", "queued"])
def test_a_transfer_keeps_neither_the_machine_nor_the_controller_alive(
    u575: popcorn.PartDescription, queued: bool
) -> None:
    device = SpiController(sck_hz=1_000_000)
    m = idle_machine(u575, device)
    asyncio.run(device.transfer([1, 2], at=m.time + (ms(5) if queued else us(1))))
    if not queued:
        asyncio.run(m.run(for_=us(20)))
        assert device.received == [0xFF, 0xFF]
    watched = weakref.ref(m), weakref.ref(device)
    del m, device
    gc.collect()
    assert [w() for w in watched] == [None, None]


@pytest.mark.parametrize(
    ("frames", "at", "error"),
    [
        ([256], None, ValueError),
        ([], None, ValueError),
        ([0] * 4097, None, ValueError),
        ([-1], None, ValueError),
        ("ab", None, TypeError),
        ([True], None, TypeError),
        ([1.0], None, TypeError),
        (5, None, TypeError),
        ([1], 5, TypeError),
        ([1], VirtualTime(-1), ValueError),
        ([1], VirtualTime(1 << 64), ValueError),
        ([1, 2, 256], None, ValueError),
    ],
    ids=[
        "above-bits",
        "empty",
        "too-many",
        "negative",
        "str",
        "bool",
        "float",
        "not-sequence",
        "at-type",
        "at-negative",
        "at-overflow",
        "last-bad",
    ],
)
def test_a_bad_transfer_is_refused_at_the_call_with_nothing_posted(
    u575: popcorn.PartDescription, frames: object, at: object, error: type[Exception]
) -> None:
    device = SpiController(sck_hz=1_000_000)
    m = idle_machine(u575, device)
    events = m.pending_events
    with pytest.raises(error):
        device.transfer(cast(list[int], frames), at=cast(VirtualTime | None, at))
    assert m.pending_events == events
    asyncio.run(m.run(for_=us(20)))
    assert device.received == []


@pytest.mark.parametrize(
    ("sck_hz", "bits", "error"),
    [
        (0, 8, ValueError),
        (100_000_001, 8, ValueError),
        (1_000_000, 3, ValueError),
        (1_000_000, 33, ValueError),
        (1.5, 8, TypeError),
        (1_000_000, True, TypeError),
    ],
)
def test_a_bad_controller_is_refused(sck_hz: object, bits: object, error: type[Exception]) -> None:
    with pytest.raises(error):
        SpiController(sck_hz=cast(int, sck_hz), bits=cast(int, bits))


def test_a_past_start_or_a_full_queue_is_refused_from_the_await_with_nothing_queued(
    u575: popcorn.PartDescription,
) -> None:
    device = SpiController(sck_hz=1_000_000)
    m = idle_machine(u575, device)
    asyncio.run(m.run(for_=us(10)))
    events = m.pending_events
    with pytest.raises(ValueError, match="before the machine time"):
        asyncio.run(device.transfer([1], at=VirtualTime(0)))
    assert m.pending_events == events
    start = m.time + ms(1)
    assert asyncio.run(device.transfer([0] * 4096, at=start)) == start
    with pytest.raises(ConfigError, match="at most 4096 SPI frames queued"):
        asyncio.run(device.transfer([1]))
    assert m.pending_events == events + 1
    asyncio.run(m.run(for_=ms(1) + VirtualDuration(4096 * FRAME_PS)))
    assert len(device.received) == 4096
    assert m.pending_events == events


def test_a_thousand_transfers_leave_no_event_and_queue_nothing(
    u575: popcorn.PartDescription,
) -> None:
    device = SpiController(sck_hz=1_000_000)
    m = idle_machine(u575, device)
    events = m.pending_events
    for _ in range(1000):
        asyncio.run(device.transfer([1]))
        asyncio.run(m.run(for_=us(9)))
    assert m.pending_events == events
    assert len(device.received) == 1000
    assert asyncio.run(device.transfer([0] * 4096)) == m.time


@pytest.mark.parametrize(
    "devices",
    [
        [SpiController(sck_hz=1_000_000), SpiTarget("PA4")],
        [SpiTarget("PA4"), SpiController(sck_hz=1_000_000)],
        [SpiController(sck_hz=1_000_000), SpiController(sck_hz=1_000_000)],
    ],
    ids=["controller-first", "target-first", "two-controllers"],
)
def test_a_controller_next_to_another_device_is_refused_naming_the_spi(
    u575: popcorn.PartDescription, devices: list[SpiController | SpiTarget]
) -> None:
    with pytest.raises(ConfigError, match=r"devices\.SPI1: one SpiController alone on SPI1"):
        popcorn.Machine(u575, devices={"SPI1": devices})


def test_a_controller_on_an_i2c_or_serving_no_machine_is_refused(
    u575: popcorn.PartDescription,
) -> None:
    device = SpiController(sck_hz=1_000_000)
    with pytest.raises(ConfigError, match="an SPI peripheral for an SpiController"):
        popcorn.Machine(u575, devices={"I2C1": [device]})
    with pytest.raises(ConfigError, match="serves no live machine"):
        device.transfer([1])
    m = idle_machine(u575, device)
    asyncio.run(device.transfer([1]))
    watched = weakref.ref(m)
    del m
    gc.collect()
    assert watched() is None
    with pytest.raises(ConfigError, match="serves no live machine"):
        device.transfer([1])


def test_a_project_wires_a_controller_and_keys_it_in_the_devices_digest(
    u575: popcorn.PartDescription,
) -> None:
    project = popcorn.MachineDescription(
        part="STM32U575QGIx",
        images=[popcorn.Elf(SPI_ELF)],
        boot=popcorn.Boot(until="main", budget=BUDGET),
        board={"SPI1": popcorn.Periph(devices=[popcorn.Device(SpiController, sck_hz=1_000_000)])},
    )
    m = project.machine()
    asyncio.run(project.boot_machine(m))
    m.reg.pc = m.sym("target_echo")
    (device,) = m.board.devices("SPI1")
    assert isinstance(device, SpiController)
    assert device.sck_hz == 1_000_000
    assert device.bits == 8
    asyncio.run(device.transfer([1, 2, 3, 4], at=m.time + START))
    assert finish(m) == ECHO_LOG
    assert device.received == ECHOED


class Evented(Stamped):
    def __init__(self) -> None:
        super().__init__()
        self.event = asyncio.Event()
        self.handler = lambda: None


def test_a_controller_is_keyed_in_the_devices_digest_by_its_class_and_source_only(
    u575: popcorn.PartDescription,
) -> None:
    project = popcorn.MachineDescription(part="STM32U575QGIx", images=[popcorn.Elf(SPI_ELF)])

    def digest(device: SpiController) -> str | None:
        return devices_digest(project, {"SPI1": [device]})

    plain = digest(SpiController(sck_hz=1_000_000))
    assert plain is not None
    assert plain == digest(SpiController(sck_hz=2_000_000, bits=16))
    stamped = digest(Stamped())
    assert stamped is not None
    assert stamped != plain
    assert digest(Evented()) not in (None, stamped)
    assert digest(Evented()) == digest(Evented())
    m, device = echo_machine(u575)
    asyncio.run(device.transfer([1, 2, 3, 4], at=m.time + START))
    assert finish(m) == ECHO_LOG
    assert device.received == ECHOED
    assert digest(device) == stamped


@pytest.mark.parametrize(
    ("entry", "match"),
    [
        (popcorn.Device(SpiController, sck_hz=1_000_000, cs="PA4"), "no cs for an SPI controller"),
        (
            popcorn.Device(SpiController, sck_hz=1_000_000, address=0x50),
            "no address for an SPI controller",
        ),
        (popcorn.Device(SpiController, speed=5), "speed"),
    ],
    ids=["cs", "address", "unknown-keyword"],
)
def test_a_bad_project_controller_is_refused_with_its_key(
    entry: popcorn.Device, match: str
) -> None:
    project = popcorn.MachineDescription(
        part="STM32U575QGIx",
        images=[popcorn.Elf(SPI_ELF)],
        board={"SPI1": popcorn.Periph(devices=[entry])},
    )
    with pytest.raises(ConfigError, match=match):
        project.machine()


def test_a_huge_transfer_is_refused_by_its_length_before_its_frames_are_read() -> None:
    class Huge(Sequence[int]):
        def __len__(self) -> int:
            return 10**7

        @overload
        def __getitem__(self, index: int) -> int: ...

        @overload
        def __getitem__(self, index: slice) -> Sequence[int]: ...

        def __getitem__(self, index: int | slice) -> int | Sequence[int]:
            raise AssertionError("a frame was read")

    with pytest.raises(ValueError, match="1 to 4096 frames, not 10000000"):
        SpiController(sck_hz=1_000_000).transfer(Huge())


def test_a_controller_of_a_collected_machine_is_not_called_and_stops_the_run(
    u575: popcorn.PartDescription,
) -> None:
    device = SpiController(sck_hz=1_000_000)
    m = idle_machine(u575, device)
    asyncio.run(device.transfer([1, 2], at=m.time + us(1)))
    del m._model_context  # pyright: ignore[reportPrivateUsage]
    stop = asyncio.run(m.run(for_=us(20)))
    assert device.received == []
    assert stop.reason is StopReason.USER
    assert stop.detail == COLLECTED


class Holding(SpiController):
    def __init__(self) -> None:
        super().__init__(sck_hz=1_000_000)
        self.machine: popcorn.Machine | None = None


def test_a_controller_holding_its_machine_is_reported_and_the_cycle_collected(
    u575: popcorn.PartDescription,
) -> None:
    device = Holding()
    m = idle_machine(u575, device)
    device.machine = m
    handle = m._handle  # pyright: ignore[reportPrivateUsage]
    assert gc.get_referents(handle).count(device) == 1
    asyncio.run(device.transfer([1, 2], at=m.time + us(1)))
    asyncio.run(m.run(for_=us(20)))
    assert device.received == [0xFF, 0xFF]
    watched = weakref.ref(m), weakref.ref(device)
    del m, device, handle
    gc.collect()
    assert [w() for w in watched] == [None, None]


class Broken(SpiController):
    def on_frame(self, sent: int, received: int, at: VirtualTime) -> None:
        raise RuntimeError("frame broke")


def test_an_on_frame_exception_stops_the_run_and_a_restore_recovers(
    u575: popcorn.PartDescription,
) -> None:
    device = Broken(sck_hz=1_000_000)
    m, _ = echo_machine(u575, device)
    before = m.snapshot()
    asyncio.run(device.transfer([1, 2, 3, 4], at=m.time + START))
    with pytest.raises(RuntimeError, match="frame broke") as raised:
        asyncio.run(m.run(budget=BUDGET))
    assert raised.traceback[-1].name == "on_frame"
    m.restore(before)
    assert finish_idle(m) == []


def test_an_on_frame_exception_keeps_neither_the_machine_nor_the_controller_alive(
    u575: popcorn.PartDescription,
) -> None:
    device = Broken(sck_hz=1_000_000)
    m = echo_machine(u575, device)[0]
    asyncio.run(device.transfer([1, 2, 3, 4], at=m.time + START))
    with pytest.raises(RuntimeError, match="frame broke") as raised:
        asyncio.run(m.run(budget=BUDGET))
    watched = weakref.ref(m), weakref.ref(device)
    del m, device, raised
    gc.collect()
    assert [w() for w in watched] == [None, None]


def finish_idle(m: popcorn.Machine) -> list[int]:
    asyncio.run(m.run(for_=ms(2)))
    return log(m)


def test_a_controller_serves_its_second_machine_once_the_first_is_gone(
    u575: popcorn.PartDescription,
) -> None:
    device = Stamped()
    first, _ = echo_machine(u575, device)
    with pytest.raises(ConfigError, match="not serving another live machine"):
        popcorn.Machine(u575, devices={"SPI1": [device]})
    del first
    gc.collect()
    second, _ = echo_machine(u575, device)
    at = second.time + START
    asyncio.run(device.transfer([1, 2, 3, 4], at=at))
    assert finish(second) == ECHO_LOG
    check_frames(device, at)


@pytest.mark.parametrize(
    ("mode", "error"),
    [(4, ValueError), (-1, ValueError), (True, TypeError), ("0", TypeError), (None, None)],
)
def test_a_controller_takes_an_spi_mode_0_to_3_or_none(
    mode: object, error: type[Exception] | None
) -> None:
    if error is None:
        SpiController(sck_hz=1_000_000, mode=cast(int | None, mode))
        return
    with pytest.raises(error, match="mode"):
        SpiController(sck_hz=1_000_000, mode=cast(int | None, mode))


def test_a_controller_declaring_the_firmware_spi_mode_clocks_as_one_declaring_none(
    u575: popcorn.PartDescription,
) -> None:
    m, device = echo_machine(u575, Stamped(mode=0))
    before = gil_entries(m)
    asyncio.run(device.transfer([1, 2, 3, 4], at=m.time + START))
    assert finish(m) == ECHO_LOG
    assert device.received == ECHOED
    assert gil_entries(m) - before == 4


def test_a_controller_declaring_another_spi_mode_fails_the_run_at_the_first_frame(
    u575: popcorn.PartDescription,
) -> None:
    m, device = echo_machine(u575, Stamped(mode=3))
    asyncio.run(device.transfer([1, 2, 3, 4], at=m.time + START))
    expected = (
        r"devices\.SPI1, expected the SPI mode 3 the controller declares, "
        r"not mode 0 the firmware set, CPOL 0 CPHA 0"
    )
    with pytest.raises(ConfigError, match=expected):
        asyncio.run(m.run(budget=BUDGET))
    assert device.received == []
    assert finish(m) == ECHO_LOG
    assert device.received == ECHOED


def test_a_machine_restored_after_a_controller_mode_mismatch_resumes_without_a_second_error(
    u575: popcorn.PartDescription,
) -> None:
    m, device = echo_machine(u575, Stamped(mode=3))
    asyncio.run(device.transfer([1, 2, 3, 4], at=m.time + START))
    with pytest.raises(ConfigError, match="the SPI mode 3 the controller declares"):
        asyncio.run(m.run(budget=BUDGET))
    saved = popcorn.Snapshot.from_bytes(m.snapshot().to_bytes())
    other, copy = echo_machine(u575, Stamped(mode=3))
    other.restore(saved)
    assert finish(other) == ECHO_LOG
    assert copy.received == ECHOED
