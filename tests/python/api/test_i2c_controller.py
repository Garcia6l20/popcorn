"""popcorn.board.I2CController: a host controller addressing an I2C the firmware runs in target
mode, on the U575, the H573 and the L152RE."""

from __future__ import annotations

import asyncio
import gc
import struct
import weakref
from collections.abc import Callable
from dataclasses import dataclass
from pathlib import Path
from typing import cast

import popcorn
import pytest
from popcorn import ConfigError, StopReason
from popcorn._refs import Pending
from popcorn.board import I2CAnswer, I2CController, I2CPhase, I2CTarget
from popcorn.pytest._cache import devices_digest
from popcorn.units import VirtualDuration, VirtualTime, ms, ps, us

from .conftest import run_bounded, stall, stalled
from .test_host_binding import COLLECTED

PREBUILT = Path(__file__).parents[2] / "firmware" / "periph" / "prebuilt"
OWN = 0x42
PHASE = us(90)
SETTLE = us(200)
WRITE_LOG = [0xADD00084, 0xDA7A0011, 0xDA7A00EE, 0xDA7A0022, 0x5709]
READ_LOG = [0xADD00085, 0x4AC0, 0x5709]
WRITE_ACKS = [True, True, True, False]
READ_BYTES = [0xB0, 0xB1, 0xB2]
L152RE_TRACED_ADDR_CLEAR = ps(10_149_383)
L152RE_MSI_INSTRUCTION = ps(476_837)
L152RE_ADDR_CLEAR_BLOCK = L152RE_MSI_INSTRUCTION * 16


@dataclass(frozen=True, slots=True)
class Part:
    description: popcorn.PartDescription
    elf: Path
    reset_register: str
    write_log: list[int]
    read_log: list[int]
    address_clear: VirtualDuration

    def machine(self, device: object) -> popcorn.Machine:
        """A machine of this part with `device` on I2C1, the target firmware listening."""
        m = popcorn.Machine(
            self.description,
            image=popcorn.Image.elf(self.elf),
            devices={"I2C1": [cast(I2CController, device)]},
        )
        m.reset()
        m.reg.pc = m.sym("target")
        asyncio.run(m.run(for_=SETTLE))
        return m

    def pulse_i2c_reset(self, m: popcorn.Machine) -> None:
        """Set and clear I2C1RST, a reset of the I2C model alone."""
        rcc = next(p for p in self.description.peripherals if p.name == "RCC")
        field = next(f for f in rcc.register(self.reset_register).fields if f.name == "I2C1RST")
        register = getattr(m.periph.RCC, self.reset_register)
        register.write(1 << field.lsb)
        register.write(0)


PARTS = {
    "u575": ("STM32U575QGIx", PREBUILT / "i2c_ll.elf", "APB1RSTR1", WRITE_LOG, READ_LOG, us(10)),
    "h573": (
        "STM32H573AIIx",
        PREBUILT / "h573" / "i2c_ll.elf",
        "APB1LRSTR",
        WRITE_LOG,
        READ_LOG,
        us(10),
    ),
    "l152re": (
        "STM32L152RETx",
        PREBUILT / "l152xe" / "l1_i2c_ll.elf",
        "APB1RSTR",
        WRITE_LOG[:-1],
        READ_LOG[:-1],
        L152RE_TRACED_ADDR_CLEAR + L152RE_ADDR_CLEAR_BLOCK,
    ),
}


@pytest.fixture(scope="session", params=sorted(PARTS))
def part(request: pytest.FixtureRequest) -> Part:
    """Each part with I2C target mode in turn, the test skipped without its description."""
    from popcorn.stm32 import mcu
    from popcorn.stm32.description import DescriptionError

    name, elf, reset_register, write_log, read_log, address_clear = PARTS[str(request.param)]
    try:
        return Part(mcu(name), elf, reset_register, write_log, read_log, address_clear)
    except DescriptionError as error:
        pytest.skip(f"no {name} description: {error}")


def log(m: popcorn.Machine) -> list[int]:
    count = m.mem.u32[m.sym("log_len")]
    raw = m.mem.read(m.sym("log_buf"), 4 * min(count, 256))
    return list(struct.unpack(f"<{len(raw) // 4}I", raw))


def gil_entries(m: popcorn.Machine) -> int:
    return m._handle.gil_entries()  # pyright: ignore[reportPrivateUsage]


def transactions(device: I2CController, base: VirtualTime) -> list[Pending[VirtualTime]]:
    """Post a write of 0x11, 0xEE, 0x22 to OWN and a read of three bytes from it, each with its
    STOP 100 us past its last phase end, at times fixed from `base`."""
    written = base + PHASE * 4 + us(100)
    read_at = written + us(10)
    return [
        device.start(OWN, at=base),
        device.write(b"\x11\xee\x22", at=base),
        device.stop(at=written),
        device.start(OWN, read=True, at=read_at),
        device.read(3, at=read_at),
        device.stop(at=read_at + PHASE * 4 + us(100)),
    ]


async def gathered(pending: list[Pending[VirtualTime]]) -> list[VirtualTime]:
    return [await one for one in pending]


def check_answers(answers: list[I2CAnswer]) -> None:
    assert [a.phase for a in answers] == [I2CPhase.ADDRESS] + [I2CPhase.WRITE] * 3 + [
        I2CPhase.ADDRESS
    ] + [I2CPhase.READ] * 3
    assert [a.ack for a in answers[:4]] == WRITE_ACKS
    assert [a.byte for a in answers[5:]] == READ_BYTES
    assert [a.ack for a in answers[4:]] == [True, True, True, False]
    assert [a.unit for a in answers] == [1, 2, 3, 4, 6, 7, 8, 9]
    assert all(a.error is None for a in answers)
    assert [a.at for a in answers] == sorted(a.at for a in answers)


def test_a_controller_writes_and_reads_the_target_firmware(part: Part) -> None:
    device = I2CController()
    m = part.machine(device)
    events = m.pending_events
    base = m.time + us(10)
    assert asyncio.run(gathered(transactions(device, base))) == [base] * 2 + [
        base + PHASE * 4 + us(100)
    ] + [base + PHASE * 4 + us(110)] * 2 + [base + PHASE * 8 + us(210)]
    asyncio.run(m.run(for_=ms(2)))
    assert log(m) == part.write_log + part.read_log
    check_answers(device.answers)
    assert device.answers[0].at > base + PHASE
    assert device.answers[0].at < base + PHASE + part.address_clear
    assert m.pending_events == events


def test_a_transaction_is_alike_parked_and_running(part: Part) -> None:
    parked = I2CController()
    m = part.machine(parked)
    base = m.time + us(10)
    asyncio.run(gathered(transactions(parked, base)))
    asyncio.run(m.run(for_=ms(2)))
    parked_log = log(m)
    running = I2CController()
    m = part.machine(running)
    assert m.time + us(10) == base
    held = stall(m, m.time + us(1))

    async def post(boundary: VirtualTime) -> list[VirtualTime]:
        assert boundary < base
        return await gathered(transactions(running, base))

    async def body() -> None:
        async with m.running():
            _, starts = await stalled(m, post, held=held)
            assert starts[0] == base
            heard = await m.wait_until(lambda _: len(running.answers) == 8, for_=ms(5))
            assert heard is not None
            await m.wait_until(lambda m: log(m) == parked_log, for_=ms(1))

    run_bounded(body(), 10)
    assert log(m) == parked_log == part.write_log + part.read_log
    assert running.answers == parked.answers
    check_answers(running.answers)


def test_each_answer_takes_the_gil_once_and_a_run_without_phases_none(part: Part) -> None:
    device = I2CController()
    m = part.machine(device)
    before = gil_entries(m)
    asyncio.run(m.run(for_=ms(1)))
    assert gil_entries(m) == before
    asyncio.run(gathered(transactions(device, m.time + us(10))))
    asyncio.run(m.run(for_=ms(2)))
    assert len(device.answers) == 8
    assert gil_entries(m) - before == 8


@pytest.mark.parametrize("read", [False, True], ids=["write", "read"])
@pytest.mark.parametrize("kind", ["i2c", "machine"])
def test_a_reset_mid_transaction_answers_every_phase_the_dropped_ones_undriven(
    part: Part, kind: str, read: bool
) -> None:
    device = I2CController()
    m = part.machine(device)
    base = m.time + us(10)
    device.start(OWN, read=read, at=base)
    if read:
        device.read(3, at=base)
    else:
        device.write([1, 2, 3], at=base)
    asyncio.run(m.run(for_=us(210)))
    reset_at = m.time
    before = gil_entries(m)
    if kind == "i2c":
        part.pulse_i2c_reset(m)
    else:
        m.reset()
    asyncio.run(m.run(for_=ms(1)))
    assert gil_entries(m) - before == 2
    answers = device.answers
    assert len(answers) == 4
    assert [a.unit for a in answers] == [1, 2, 3, 4]
    assert answers[0].ack
    assert answers[1].ack
    if read:
        assert answers[1].byte == READ_BYTES[0]
    for dropped in answers[2:]:
        assert (dropped.ack, dropped.byte, dropped.error) == (False, 0xFF, None)
    assert answers[2].at.ps > (0 if kind == "machine" else reset_at.ps)
    if kind == "machine":
        assert answers[2].at == VirtualTime(answers[1].at.ps + PHASE.ps - reset_at.ps)
    else:
        assert answers[2].at == answers[1].at + PHASE
    assert answers[3].at == answers[2].at + PHASE


def test_a_stop_inside_a_phase_drops_it_and_its_answer_is_undriven(part: Part) -> None:
    device = I2CController()
    m = part.machine(device)
    base = m.time + us(10)
    device.start(OWN, at=base)
    device.write([0x11, 0x22], at=base)
    device.stop(at=base + PHASE * 2 + us(30))
    asyncio.run(m.run(for_=ms(1)))
    assert [(a.unit, a.ack) for a in device.answers] == [(1, True), (2, True), (3, False)]
    assert device.answers[2].at == base + PHASE * 2 + us(30)
    assert log(m) == [0xADD00084, 0xDA7A0011, 0x5709]


def test_a_restored_snapshot_numbers_new_phases_after_its_queued_ones(part: Part) -> None:
    first = I2CController()
    m = part.machine(first)
    base = m.time + ms(1)
    asyncio.run(first.start(OWN, at=base))
    asyncio.run(first.write([0x11], at=base))
    saved = popcorn.Snapshot.from_bytes(m.snapshot().to_bytes())
    second = I2CController()
    other = part.machine(second)
    other.restore(saved)
    asyncio.run(second.write([0x22]))
    asyncio.run(second.stop(at=base + PHASE * 3 + us(100)))
    asyncio.run(other.run(for_=ms(2)))
    assert [(a.unit, a.phase, a.ack) for a in second.answers] == [
        (1, I2CPhase.ADDRESS, True),
        (2, I2CPhase.WRITE, True),
        (3, I2CPhase.WRITE, True),
    ]
    assert first.answers == []
    assert log(other) == [0xADD00084, 0xDA7A0011, 0xDA7A0022, 0x5709]


@pytest.mark.parametrize("queued", [False, True], ids=["answered", "queued"])
def test_a_transaction_keeps_neither_the_machine_nor_the_controller_alive(
    part: Part, queued: bool
) -> None:
    device = I2CController()
    m = part.machine(device)
    asyncio.run(device.start(OWN, at=m.time + (ms(5) if queued else us(1))))
    if not queued:
        asyncio.run(m.run(for_=ms(1)))
        assert len(device.answers) == 1
    watched = weakref.ref(m), weakref.ref(device)
    del m, device
    gc.collect()
    assert [w() for w in watched] == [None, None]


CALLS: dict[str, Callable[[I2CController], object]] = {
    "address-high": lambda c: c.start(0x80),
    "address-negative": lambda c: c.start(-1),
    "address-bool": lambda c: c.start(cast(int, True)),
    "address-float": lambda c: c.start(cast(int, 1.0)),
    "read-int": lambda c: c.start(OWN, read=cast(bool, 1)),
    "at-type": lambda c: c.start(OWN, at=cast(VirtualTime, 5)),
    "at-negative": lambda c: c.start(OWN, at=VirtualTime(-1)),
    "at-far": lambda c: c.stop(at=VirtualTime((1 << 63) + 1)),
    "write-empty": lambda c: c.write(b""),
    "write-many": lambda c: c.write(bytes(4097)),
    "write-high": lambda c: c.write([0x100]),
    "write-last-bad": lambda c: c.write([1, 2, -1]),
    "write-str": lambda c: c.write(cast(bytes, "ab")),
    "write-bool": lambda c: c.write([cast(int, True)]),
    "write-not-sequence": lambda c: c.write(cast(bytes, 5)),
    "read-zero": lambda c: c.read(0),
    "read-many": lambda c: c.read(4097),
    "read-float": lambda c: c.read(cast(int, 1.0)),
    "ack-last-int": lambda c: c.read(1, ack_last=cast(bool, 0)),
}

CALL_ERRORS: dict[str, type[Exception]] = {
    "address-high": ValueError,
    "address-negative": ValueError,
    "address-bool": TypeError,
    "address-float": TypeError,
    "read-int": TypeError,
    "at-type": TypeError,
    "at-negative": ValueError,
    "at-far": ValueError,
    "write-empty": ValueError,
    "write-many": ValueError,
    "write-high": ValueError,
    "write-last-bad": ValueError,
    "write-str": TypeError,
    "write-bool": TypeError,
    "write-not-sequence": TypeError,
    "read-zero": ValueError,
    "read-many": ValueError,
    "read-float": TypeError,
    "ack-last-int": TypeError,
}


@pytest.mark.parametrize("call", sorted(CALLS))
def test_a_bad_call_is_refused_at_the_call_with_nothing_posted(part: Part, call: str) -> None:
    device = I2CController()
    m = part.machine(device)
    events = m.pending_events
    with pytest.raises(CALL_ERRORS[call]):
        CALLS[call](device)
    assert m.pending_events == events
    asyncio.run(m.run(for_=ms(1)))
    assert device.answers == []


@pytest.mark.parametrize(
    ("scl_hz", "error"),
    [(999, ValueError), (5_000_001, ValueError), (1.5, TypeError), (True, TypeError)],
)
def test_a_bad_controller_is_refused(scl_hz: object, error: type[Exception]) -> None:
    with pytest.raises(error):
        I2CController(scl_hz=cast(int, scl_hz))


def test_the_phase_time_is_nine_scl_periods() -> None:
    assert I2CController().phase_time == PHASE
    assert I2CController(scl_hz=400_000).phase_time == VirtualDuration(22_500_000)


def test_a_past_start_or_a_full_port_is_refused_from_the_await_with_nothing_queued(
    part: Part,
) -> None:
    device = I2CController()
    m = part.machine(device)
    events = m.pending_events
    with pytest.raises(ValueError, match="before the machine time"):
        asyncio.run(device.start(OWN, at=VirtualTime(0)))
    assert m.pending_events == events
    far = m.time + ms(500)
    asyncio.run(device.write(bytes(4000), at=far))
    assert m.pending_events == events + 4000
    with pytest.raises(ConfigError, match="at most 4096 I2C port entries queued"):
        asyncio.run(device.write(bytes(97), at=far))
    assert m.pending_events == events + 4000
    asyncio.run(device.write(bytes(96), at=far))
    assert m.pending_events == events + 4096
    with pytest.raises(ConfigError, match="at most 4096 I2C port entries queued"):
        asyncio.run(device.stop(at=far))


@pytest.mark.parametrize(
    "devices",
    [
        [I2CController(), I2CTarget(0x50)],
        [I2CTarget(0x50), I2CController()],
        [I2CController(), I2CController()],
    ],
    ids=["controller-first", "target-first", "two-controllers"],
)
def test_a_controller_next_to_another_device_is_refused_naming_the_i2c(
    part: Part, devices: list[I2CController | I2CTarget]
) -> None:
    with pytest.raises(ConfigError, match=r"devices\.I2C1: one I2CController alone on I2C1"):
        popcorn.Machine(part.description, devices={"I2C1": devices})


def test_a_controller_on_an_spi_serving_no_machine_or_another_is_refused(part: Part) -> None:
    device = I2CController()
    with pytest.raises(ConfigError, match="an I2C peripheral for an I2CController"):
        popcorn.Machine(part.description, devices={"SPI1": [device]})
    with pytest.raises(ConfigError, match="serves no live machine"):
        device.start(OWN)
    m = part.machine(device)
    with pytest.raises(ConfigError, match="not serving another live machine"):
        popcorn.Machine(part.description, devices={"I2C1": [device]})
    asyncio.run(device.start(OWN))
    watched = weakref.ref(m)
    del m
    gc.collect()
    assert watched() is None
    with pytest.raises(ConfigError, match="serves no live machine"):
        device.stop()
    second = part.machine(device)
    base = second.time + us(10)
    asyncio.run(gathered(transactions(device, base)))
    asyncio.run(second.run(for_=ms(2)))
    assert log(second) == part.write_log + part.read_log


def test_a_project_wires_a_controller_and_keys_it_by_its_class(part: Part) -> None:
    project = popcorn.MachineDescription(
        part=part.description.part,
        images=[popcorn.Elf(part.elf)],
        board={"I2C1": popcorn.Periph(devices=[popcorn.Device(I2CController, scl_hz=400_000)])},
    )
    m = project.machine()
    (device,) = m.board.devices("I2C1")
    assert isinstance(device, I2CController)
    assert device.scl_hz == 400_000
    m.reset()
    m.reg.pc = m.sym("target")
    asyncio.run(m.run(for_=SETTLE))
    asyncio.run(device.start(OWN))
    asyncio.run(m.run(for_=us(100)))
    assert [a.ack for a in device.answers] == [True]
    plain = devices_digest(project, {"I2C1": [I2CController()]})
    assert plain is not None
    assert plain == devices_digest(project, {"I2C1": [I2CController(scl_hz=400_000)]})


@pytest.mark.parametrize(
    ("peripheral", "entry", "match", "key"),
    [
        (
            "I2C1",
            popcorn.Device(I2CController, cs="PA4"),
            "no cs for an I2C controller",
            "board.I2C1.devices[0].cs",
        ),
        (
            "I2C1",
            popcorn.Device(I2CController, address=0x50),
            "no address for an I2C controller",
            "board.I2C1.devices[0].address",
        ),
        ("SPI1", popcorn.Device(I2CController), "an I2C bus of", "board.SPI1.devices[0].model"),
        ("I2C1", popcorn.Device(I2CController, speed=5), "speed", "board.I2C1.devices[0].speed"),
    ],
    ids=["cs", "address", "on-spi", "unknown-keyword"],
)
def test_a_bad_project_controller_is_refused_with_its_key(
    part: Part, peripheral: str, entry: popcorn.Device, match: str, key: str
) -> None:
    project = popcorn.MachineDescription(
        part=part.description.part,
        images=[popcorn.Elf(part.elf)],
        board={peripheral: popcorn.Periph(devices=[entry])},
    )
    with pytest.raises(ConfigError, match=match) as caught:
        project.machine()
    assert caught.value.key == key


def test_a_controller_of_a_collected_machine_is_not_called_and_stops_the_run(part: Part) -> None:
    device = I2CController()
    m = part.machine(device)
    asyncio.run(device.start(OWN, at=m.time + us(1)))
    del m._model_context  # pyright: ignore[reportPrivateUsage]
    stop = asyncio.run(m.run(for_=ms(1)))
    assert device.answers == []
    assert stop.reason is StopReason.USER
    assert stop.detail == COLLECTED


class Broken(I2CController):
    def on_answer(self, answer: I2CAnswer) -> None:
        raise RuntimeError("answer broke")


def test_an_on_answer_exception_stops_the_run_and_keeps_nothing_alive(part: Part) -> None:
    device = Broken()
    m = part.machine(device)
    asyncio.run(device.start(OWN, at=m.time + us(1)))
    with pytest.raises(RuntimeError, match="answer broke") as raised:
        asyncio.run(m.run(for_=ms(1)))
    assert raised.traceback[-1].name == "on_answer"
    watched = weakref.ref(m), weakref.ref(device)
    del m, device, raised
    gc.collect()
    assert [w() for w in watched] == [None, None]
