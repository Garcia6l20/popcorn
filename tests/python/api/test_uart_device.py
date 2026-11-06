"""A device on the far end of a UART: frames on the machine's thread at their stamps, sends at the
firmware's baud, on every USART row."""

from __future__ import annotations

import asyncio
import threading
import weakref
from collections.abc import Awaitable, Callable, Mapping, Sequence
from dataclasses import dataclass
from functools import partial
from pathlib import Path

import popcorn
import pytest
from popcorn import System, UartLink, _core
from popcorn.arm import Reg
from popcorn.board import UartDevice, UartError
from popcorn.units import VirtualDuration, VirtualTime, ms, ps, us

from .conftest import BOARD_ELF, collected

WFI_LOOP = b"\x30\xbf\xfd\xe7"
NOP = b"\x00\xbf"
BRANCH_BACK_40_NOPS = b"\xd6\xe7"
READ_BLOCK_NOPS = 40
LDR_R0_R1 = b"\x08\x68"
BRANCH_BACK_READ_BLOCK = b"\xd5\xe7"
AIRCR_RESET = bytes.fromhex("0148" "0249" "0160" "fee7" "0ced00e0" "0400fa05")
BRANCH_TO_SELF = b"\xfe\xe7"
UE_CLEAR_AND_SET = bytes.fromhex("0348" "0021" "0160" "0349" "0160" "30bf" "fde7" "00bf")
UE_TE_RE = (0xD).to_bytes(4, "little")
RAM = 0x2000_0000
THUMB = 1 << 24
BRR = 35


class Recorder(UartDevice):
    def __init__(self) -> None:
        super().__init__()
        self.frames: list[tuple[int, int]] = []
        self.errors: list[tuple[UartError, int]] = []
        self.resets: list[popcorn.ResetKind] = []
        self.attaches: list[int] = []
        self.nows: list[int] = []

    def on_attach(self) -> None:
        self.attaches.append(threading.get_ident())

    def on_frame(self, byte: int, at: VirtualTime) -> None:
        self.frames.append((byte, at.ps))
        self.nows.append(self.now.ps)

    def on_error(self, kind: UartError, at: VirtualTime) -> None:
        self.errors.append((kind, at.ps))

    def reset(self, kind: popcorn.ResetKind) -> None:
        self.resets.append(kind)


class Replier(Recorder):
    """Answers each frame `after` the frame's stop-bit end."""

    def __init__(self, after: VirtualDuration) -> None:
        super().__init__()
        self.after = after

    def on_frame(self, byte: int, at: VirtualTime) -> None:
        super().on_frame(byte, at)
        self.send(bytes([byte ^ 0xFF]), at=at + self.after)


class Starter(Recorder):
    """Sends `data` when attached, a frame `at` from the current time."""

    def __init__(self, data: bytes, after_us: int = 0) -> None:
        super().__init__()
        self.data = data
        self.after = us(after_us)

    def on_attach(self) -> None:
        super().on_attach()
        self.send(self.data, at=self.now + self.after)


class KickedReplier(Replier):
    """A `Replier` that opens the exchange with one byte when attached."""

    def on_attach(self) -> None:
        super().on_attach()
        self.send(b"\x5a", at=self.now)


@dataclass(frozen=True)
class Row:
    part: str
    enable: str
    tx: str
    status: str
    ready: str
    rx: str
    description: popcorn.PartDescription


ROWS = {
    "u575": ("STM32U575QGIx", "CR1_disabled", "TDR", "ISR_enabled", "RXFNE", "RDR"),
    "h573": ("STM32H573AIIx", "CR1_disabled", "TDR", "ISR_enabled", "RXFNE", "RDR"),
    "l152": ("STM32L152RETx", "CR1", "DR", "SR", "RXNE", "DR"),
    "l476": ("STM32L476RGTx", "CR1", "TDR", "ISR", "RXNE", "RDR"),
}


@pytest.fixture(scope="session", params=sorted(ROWS))
def row(request: pytest.FixtureRequest) -> Row:
    from popcorn.stm32 import mcu
    from popcorn.stm32.description import DescriptionError

    part, enable, tx, status, ready, rx = ROWS[str(request.param)]
    try:
        return Row(part, enable, tx, status, ready, rx, mcu(part))
    except DescriptionError as error:
        pytest.skip(f"no {part} description: {error}")


def arm(m: popcorn.Machine, row: Row, *, brr: int = BRR, rtse: bool = False) -> None:
    """Put the core of `m` in a WFI loop that nothing wakes and enable USART1 of `row`."""
    m.mem.write(RAM, WFI_LOOP)
    m.reg[Reg.PC] = RAM
    m.reg[Reg.SP] = RAM + 0x800
    m.reg[Reg.XPSR] = THUMB
    m.reg[Reg.PRIMASK] = 1
    m.periph.RCC.APB2ENR.USART1EN = 1
    uart = m.periph.USART1
    uart.BRR.value = brr
    if rtse:
        uart.CR3.RTSE = 1
    uart[row.enable].poke(UE=1, TE=1, RE=1)


def asleep(
    row: Row,
    devices: Mapping[str, Sequence[UartDevice]] | None = None,
    *,
    brr: int = BRR,
    rtse: bool = False,
) -> popcorn.Machine:
    """A reset machine of `row`, see `arm`."""
    m = popcorn.Machine(row.description, devices=devices)
    m.reset()
    arm(m, row, brr=brr, rtse=rtse)
    return m


def frame_time(row: Row) -> int:
    """The frame time of USART1 of `row` at `BRR`, as a device hears it."""
    probe = Recorder()
    m = asleep(row, {"USART1": [probe]})
    start = m.time
    transmit(m, row, 0x41)
    asyncio.run(m.run(for_=ms(5)))
    return probe.frames[0][1] - start.ps


def received(m: popcorn.Machine, row: Row) -> bool:
    return getattr(m.periph.USART1[row.status], row.ready) != 0


def transmit(m: popcorn.Machine, row: Row, byte: int) -> None:
    m.periph.USART1[row.tx].value = byte


async def once[T](pending: Awaitable[T]) -> T:
    return await pending


def run_to(m: popcorn.Machine, stamp_ps: int) -> None:
    assert stamp_ps > m.time.ps
    asyncio.run(m.run(for_=ps(stamp_ps - m.time.ps)))


def instruction_ps(m: popcorn.Machine) -> int:
    before = m.time
    asyncio.run(m.step())
    return (m.time - before).ps


def test_frames_equal_a_stream_capture_of_the_same_run(row: Row) -> None:
    def captured() -> list[tuple[int, int]]:
        m = asleep(row)
        transmit(m, row, 0x41)
        asyncio.run(m.run(for_=ms(3)))
        return [(f.data, f.at.ps) for f in m.periph.USART1.port.take_frames()]

    device = Recorder()
    m = asleep(row, {"USART1": [device]})
    transmit(m, row, 0x41)
    asyncio.run(m.run(for_=ms(3)))
    assert device.frames == captured()
    assert len(device.frames) == 1
    assert device.nows == [device.frames[0][1]]


def test_a_reply_lands_at_its_distance_from_the_frame(row: Row) -> None:
    device = Replier(us(100))
    m = asleep(row, {"USART1": [device]})
    start = m.time
    transmit(m, row, 0x41)

    asyncio.run(m.run(for_=ms(5)))
    ((_, end_ps),) = device.frames
    frame_ps = end_ps - start.ps
    assert received(m, row)
    assert m.periph.USART1[row.rx].value & 0xFF == 0xBE

    device = Replier(us(100))
    m = asleep(row, {"USART1": [device]})
    start = m.time
    transmit(m, row, 0x41)
    step = instruction_ps(m)
    due = start.ps + frame_ps + us(100).ps + frame_ps
    run_to(m, due - 2 * step)
    assert not received(m, row)
    run_to(m, due + 2 * step)
    assert received(m, row)


def test_a_baud_change_between_frames_applies_from_the_next_one(row: Row) -> None:
    device = Starter(b"\x11\x22")
    m = asleep(row, {"USART1": [device]})
    uart = m.periph.USART1
    start = m.time
    step = instruction_ps(m)
    frame_ps = frame_time(row)
    run_to(m, start.ps + frame_ps + 2 * step)
    assert received(m, row)
    assert uart[row.rx].value & 0xFF == 0x11
    uart[row.enable if row.enable == "CR1" else "CR1_enabled"].UE = 0
    uart.BRR.value = BRR * 2
    uart[row.enable].UE = 1
    restart = m.time
    assert not received(m, row)
    run_to(m, restart.ps + frame_ps * 2 - 2 * step)
    assert not received(m, row)
    run_to(m, restart.ps + frame_ps * 2 + 2 * step)
    assert received(m, row)
    assert uart[row.rx].value & 0xFF == 0x22


def test_a_frame_after_a_ue_clear_counts_from_the_restart_at_the_new_baud(row: Row) -> None:
    device = Starter(b"\x11\x22")
    m = asleep(row, {"USART1": [device]})
    uart = m.periph.USART1
    step = instruction_ps(m)
    frame_ps = frame_time(row)
    asyncio.run(m.run(for_=us(5)))
    uart[row.enable if row.enable == "CR1" else "CR1_enabled"].UE = 0
    uart.BRR.value = BRR // 8
    uart[row.enable].UE = 1
    fast = (BRR // 8) * frame_ps // BRR
    restart = m.time
    run_to(m, restart.ps + fast + 2 * step)
    assert received(m, row)


def release_times(
    row: Row, m: popcorn.Machine, frame_ps: int, step: int, release: Callable[[], None]
) -> tuple[bool, bool]:
    """Whether the receiver has a frame 1 us and one frame after `release` runs."""
    release()
    released = m.time
    run_to(m, released.ps + us(1).ps)
    soon = received(m, row)
    run_to(m, released.ps + frame_ps + 2 * step)
    return soon, received(m, row)


def held_by_brr(row: Row, *, device: bool) -> tuple[bool, bool]:
    def set_baud() -> None:
        uart = m.periph.USART1
        uart[row.enable if row.enable == "CR1" else "CR1_enabled"].UE = 0
        uart.BRR.value = BRR
        uart[row.enable].UE = 1

    frame_ps = frame_time(row)
    m = asleep(row, {"USART1": [Starter(b"\x41", 50)]} if device else None, brr=0)
    step = instruction_ps(m)
    if not device:
        asyncio.run(once(m.periph.USART1.port.write(b"\x41")))
    asyncio.run(m.run(for_=ms(1)))
    return release_times(row, m, frame_ps, step, set_baud)


def held_by_rts(row: Row, *, device: bool) -> tuple[bool, bool]:
    class Two(Recorder):
        def on_attach(self) -> None:
            super().on_attach()
            self.send(b"\x41", at=self.now + us(1))
            self.send(b"\x42", at=self.now + us(1))

    def flush() -> None:
        uart = m.periph.USART1
        if row.status == "SR":
            uart.SR.RXNE = 0
        else:
            uart.RQR.RXFRQ = 1

    frame_ps = frame_time(row)
    m = asleep(row, {"USART1": [Two()]} if device else None, rtse=True)
    step = instruction_ps(m)
    if not device:
        asyncio.run(once(m.periph.USART1.port.write(b"\x41")))
        asyncio.run(once(m.periph.USART1.port.write(b"\x42")))
    asyncio.run(m.run(for_=ms(2)))
    assert received(m, row)
    return release_times(row, m, frame_ps, step, flush)


def test_a_frame_held_by_brr_zero_takes_its_frame_time_after_the_release(row: Row) -> None:
    assert held_by_brr(row, device=True) == held_by_brr(row, device=False) == (False, True)


def test_a_frame_held_by_rts_takes_its_frame_time_after_the_release(row: Row) -> None:
    assert held_by_rts(row, device=True) == held_by_rts(row, device=False) == (False, True)


def record_read(m: popcorn.Machine, reads: list[tuple[int, int]], hit: popcorn.WatchHit) -> None:
    reads.append((m.time.ps, hit.value))


def test_a_host_start_dispatched_a_block_late_counts_from_the_stamp(row: Row) -> None:
    """The stamp falls inside a block that ends in a read of the empty data register. That read
    returns before it could resume the queue, so the frame starts at the block's end, from the
    event at the stamp, and its due time counts from the stamp and not from the dispatch."""
    base = next(p.base for p in row.description.peripherals if p.name == "USART1")
    step = instruction_ps(asleep(row))
    frame_ps = frame_time(row)
    period = (READ_BLOCK_NOPS + 2) * step
    for offset in range(0, READ_BLOCK_NOPS, 4):
        device = Starter(b"\x41", 50)
        device.after = us(50) + ps(offset * step)
        m = asleep(row, {"USART1": [device]})
        address = base + m.periph.USART1[row.rx].offset
        m.mem.write(RAM, NOP * READ_BLOCK_NOPS + LDR_R0_R1 + BRANCH_BACK_READ_BLOCK)
        m.reg[Reg.R1] = address
        reads: list[tuple[int, int]] = []

        m.watch(address, 4, popcorn.WatchKind.READ, partial(record_read, m, reads))
        due = device.after.ps + frame_ps
        asyncio.run(m.run(for_=ps(due + 3 * period)))
        arrived = next(at for at, value in reads if value & 0xFF == 0x41)
        expected = next(at for at, _ in reads if at - period >= due)
        assert arrived == expected, (offset, due, step)


def test_a_send_behind_queued_frames_starts_when_they_end(u575: popcorn.PartDescription) -> None:
    row = Row("STM32U575QGIx", "CR1_disabled", "TDR", "ISR_enabled", "RXFNE", "RDR", u575)

    class Twice(Recorder):
        def on_attach(self) -> None:
            super().on_attach()
            self.send(b"\x01\x02", at=self.now + us(50))
            self.send(b"\x03", at=self.now + us(60))
            self.send(b"\x04", at=self.now + ms(5))

    device = Twice()
    m = asleep(row, {"USART1": [device]})
    assert m.periph.USART1.BRR.value == BRR
    start = m.time
    frame_ps = 10 * BRR * 10**12 // 4_000_000
    step = instruction_ps(m)
    begin = start.ps + us(50).ps
    for index in range(3):
        due = begin + (index + 1) * frame_ps
        run_to(m, due + 2 * step)
        assert m.periph.USART1.RDR.value & 0xFF == index + 1
        m.periph.USART1.RQR.RXFRQ = 1
        assert not received(m, row)
    gap_due = start.ps + ms(5).ps + frame_ps
    run_to(m, start.ps + ms(5).ps - 2 * step)
    assert m.periph.USART1.ISR_enabled.BUSY == 0
    run_to(m, gap_due - 2 * step)
    assert m.periph.USART1.ISR_enabled.BUSY == 1
    assert not received(m, row)
    run_to(m, gap_due + 2 * step)
    assert m.periph.USART1.RDR.value & 0xFF == 4


def test_gil_entries_equal_the_device_calls(u575: popcorn.PartDescription) -> None:
    device = Starter(b"abcdefgh")
    m = popcorn.Machine(u575, image=popcorn.Image.elf(BOARD_ELF), devices={"USART1": [device]})
    m.reset()
    asyncio.run(m.run(until=m.sym("ready"), budget=1_000_000))
    before = m._handle.gil_entries()  # pyright: ignore[reportPrivateUsage]
    asyncio.run(m.run(for_=ms(20)))
    assert len(device.frames) == 8
    assert m._handle.gil_entries() - before == 8  # pyright: ignore[reportPrivateUsage]


def test_now_is_the_frame_stamp_when_the_machine_runs_a_block_past_it(
    u575: popcorn.PartDescription,
) -> None:
    late: list[int] = []

    class Timed(Recorder):
        def on_frame(self, byte: int, at: VirtualTime) -> None:
            late.append(m.time.ps - at.ps)
            super().on_frame(byte, at)

    row = Row("STM32U575QGIx", "CR1_disabled", "TDR", "ISR_enabled", "RXFNE", "RDR", u575)
    device = Timed()
    m = asleep(row, {"USART1": [device]})
    m.mem.write(RAM, NOP * 40 + BRANCH_BACK_40_NOPS)
    transmit(m, row, 0x41)
    asyncio.run(m.run(for_=ms(1)))
    assert late[0] > 0
    assert device.nows == [device.frames[0][1]]


def test_on_attach_runs_once_on_the_machine_thread(u575: popcorn.PartDescription) -> None:
    device = Recorder()
    m = popcorn.Machine(u575, image=popcorn.Image.elf(BOARD_ELF), devices={"USART1": [device]})
    m.reset()
    assert device.attaches == []
    m.reset(popcorn.ResetKind.SOFTWARE)
    assert device.attaches == []
    asyncio.run(m.run(for_=us(10)))
    assert len(device.attaches) == 1
    assert device.attaches[0] != threading.get_ident()
    m.reset(popcorn.ResetKind.SOFTWARE)
    asyncio.run(m.run(for_=us(10)))
    assert len(device.attaches) == 1


def test_a_restored_snapshot_attaches_a_fresh_device_once(
    u575: popcorn.PartDescription, tmp_path: Path
) -> None:
    first = Recorder()
    m = popcorn.Machine(u575, image=popcorn.Image.elf(BOARD_ELF), devices={"USART1": [first]})
    m.reset()
    asyncio.run(m.run(for_=us(10)))
    saved = m.snapshot()
    path = tmp_path / "attached.snapshot"
    saved.save(path)
    second = Recorder()
    other = popcorn.Machine(u575, image=popcorn.Image.elf(BOARD_ELF), devices={"USART1": [second]})
    other.restore(other.load_snapshot(path))
    asyncio.run(other.run(for_=us(10)))
    assert len(second.attaches) == 1
    m.restore(saved)
    asyncio.run(m.run(for_=us(10)))
    assert len(first.attaches) == 1


@pytest.mark.parametrize(
    "kind", [popcorn.ResetKind.SOFTWARE, popcorn.ResetKind.PIN, popcorn.ResetKind.POWER_ON]
)
def test_every_machine_reset_reaches_the_device_and_drops_queued_frames(
    row: Row, kind: popcorn.ResetKind
) -> None:
    device = Starter(b"\x11\x22\x33", 200)
    m = asleep(row, {"USART1": [device]})
    assert device.resets == [popcorn.ResetKind.POWER_ON]
    asyncio.run(m.run(for_=us(10)))
    m.reset(kind)
    assert device.resets == [popcorn.ResetKind.POWER_ON, kind]
    arm(m, row)
    m.periph.USART1[row.enable].UE = 1
    asyncio.run(m.run(for_=ms(5)))
    assert not received(m, row)


def test_a_stamp_after_a_reset_of_running_time_uses_the_machine_time_base(row: Row) -> None:
    device = Replier(us(100))
    m = asleep(row, {"USART1": [device]})
    step = instruction_ps(m)
    frame_ps = frame_time(row)
    asyncio.run(m.run(for_=ms(1)))
    m.reset(popcorn.ResetKind.SOFTWARE)
    assert m.time.ps == 0
    arm(m, row)
    transmit(m, row, 0x41)
    run_to(m, frame_ps + us(100).ps + frame_ps - 2 * step)
    assert device.frames[0][1] == frame_ps
    assert not received(m, row)
    run_to(m, frame_ps + us(100).ps + frame_ps + 2 * step)
    assert received(m, row)


def pulse_reset_bit(m: popcorn.Machine) -> None:
    m.periph.RCC.APB2RSTR.USART1RST = 1
    m.periph.RCC.APB2RSTR.USART1RST = 0


def test_a_stamp_pending_across_the_reset_bit_keeps_its_time(row: Row) -> None:
    m = asleep(row, {"USART1": [Starter(b"\x41", 200)]})
    step = instruction_ps(m)
    frame_ps = frame_time(row)
    asyncio.run(m.run(for_=us(10)))
    pulse_reset_bit(m)
    m.periph.USART1.BRR.value = BRR
    m.periph.USART1[row.enable].UE = 1
    m.periph.USART1[row.enable].poke(UE=1, TE=1, RE=1)
    run_to(m, us(200).ps + frame_ps - 2 * step)
    assert not received(m, row)
    run_to(m, us(200).ps + frame_ps + 2 * step)
    assert received(m, row)


def test_a_stamp_gone_stale_across_the_reset_bit_takes_its_frame_time(row: Row) -> None:
    def reconfigure() -> None:
        uart = m.periph.USART1
        uart.BRR.value = BRR
        uart[row.enable].poke(TE=1, RE=1)
        uart[row.enable].UE = 1

    m = asleep(row, {"USART1": [Starter(b"\x41", 20)]})
    step = instruction_ps(m)
    frame_ps = frame_time(row)
    asyncio.run(m.run(for_=us(10)))
    pulse_reset_bit(m)
    asyncio.run(m.run(for_=ms(1)))
    assert release_times(row, m, frame_ps, step, reconfigure) == (False, True)


def test_one_device_hears_all_four_hooks(row: Row) -> None:
    device = Recorder()
    m = asleep(row, {"USART1": [device]})
    transmit(m, row, 0x41)
    asyncio.run(m.run(for_=ms(5)))
    transmit(m, row, 0x42)
    asyncio.run(m.run(for_=us(5)))
    pulse_reset_bit(m)
    assert device.resets == [popcorn.ResetKind.POWER_ON]
    assert len(device.attaches) == 1
    assert [byte for byte, _ in device.frames] == [0x41]
    assert [kind for kind, _ in device.errors] == [UartError.ABORTED]


def abort_reply(row: Row, *, firmware: bool) -> tuple[bool, bool]:
    threads: list[bool] = []

    class Answering(Recorder):
        def on_error(self, kind: UartError, at: VirtualTime) -> None:
            super().on_error(kind, at)
            threads.append(_core.on_host_thread())
            self.send(b"\x55", at=at + us(100))

    base = next(p.base for p in row.description.peripherals if p.name == "USART1")
    device = Answering()
    m = asleep(row, {"USART1": [device]})
    m.mem.write(RAM, BRANCH_TO_SELF)
    step = instruction_ps(m)
    frame_ps = frame_time(row)
    transmit(m, row, 0x41)
    asyncio.run(m.run(for_=us(5)))
    uart = m.periph.USART1
    if firmware:
        offset = uart[row.enable].offset
        m.mem.write(RAM, UE_CLEAR_AND_SET + (base + offset).to_bytes(4, "little") + UE_TE_RE)
        m.reg[Reg.PC] = RAM
        asyncio.run(m.run(for_=us(5)))
    else:
        uart[row.enable if row.enable == "CR1" else "CR1_enabled"].UE = 0
        uart[row.enable].poke(TE=1, RE=1)
        uart[row.enable].UE = 1
    assert threads == [firmware]
    due = device.errors[0][1] + us(100).ps + frame_ps
    run_to(m, due - 2 * step)
    soon = received(m, row)
    run_to(m, due + 2 * step)
    return soon, received(m, row)


def test_a_reply_from_on_error_lands_the_same_parked_and_from_firmware(row: Row) -> None:
    if row.status == "SR":
        pytest.skip("UE lets the frame finish on this row, RM0038 27.6.4")
    assert abort_reply(row, firmware=False) == abort_reply(row, firmware=True) == (False, True)


@pytest.mark.parametrize(
    ("part", "unit", "enable", "brr", "tx", "status", "ready", "rx"),
    [
        ("STM32U575QGIx", "LPUART1", "APB3ENR.LPUART1EN", 0x10000, "TDR", "ISR", "RXFNE", "RDR"),
        ("STM32L152RETx", "UART4", "APB1ENR.UART4EN", BRR, "DR", "SR", "RXNE", "DR"),
    ],
)
def test_a_reply_lands_at_its_distance_on_other_uart_ips(
    part: str,
    unit: str,
    enable: str,
    brr: int,
    tx: str,
    status: str,
    ready: str,
    rx: str,
) -> None:
    from popcorn.stm32 import mcu
    from popcorn.stm32.description import DescriptionError

    try:
        description = mcu(part)
    except DescriptionError as error:
        pytest.skip(f"no {part} description: {error}")
    device = Replier(us(100))
    m = popcorn.Machine(description, devices={unit: [device]})
    m.reset()
    m.mem.write(RAM, WFI_LOOP)
    m.reg[Reg.PC] = RAM
    m.reg[Reg.SP] = RAM + 0x800
    m.reg[Reg.XPSR] = THUMB
    m.reg[Reg.PRIMASK] = 1
    register, field = enable.split(".")
    setattr(m.periph.RCC[register], field, 1)
    uart = m.periph[unit]
    uart.BRR.value = brr
    uart.CR1.poke(UE=1, TE=1, RE=1)
    step = instruction_ps(m)
    start = m.time
    uart[tx].value = 0x41
    while not device.frames:
        asyncio.run(m.run(for_=us(20)))
    ((_, end),) = device.frames
    frame_ps = end - start.ps
    due = end + us(100).ps + frame_ps
    run_to(m, due - 2 * step)
    assert getattr(uart[status], ready) == 0
    run_to(m, due + 2 * step)
    assert getattr(uart[status], ready) == 1
    assert uart[rx].value & 0xFF == 0xBE


def test_parity_is_stripped_and_is_no_error(u575: popcorn.PartDescription) -> None:
    row = Row("STM32U575QGIx", "CR1_disabled", "TDR", "ISR_enabled", "RXFNE", "RDR", u575)
    device = Recorder()
    m = asleep(row, {"USART1": [device]})
    uart = m.periph.USART1
    uart.CR1_enabled.UE = 0
    uart.CR1_disabled.poke(UE=1, TE=1, RE=1, PCE=1)
    uart.TDR.value = 0xC1
    asyncio.run(m.run(for_=ms(1)))
    assert [byte for byte, _ in device.frames] == [0x41]
    assert device.errors == []


def test_a_frame_cut_by_the_uart_reset_bit_is_an_abort_and_no_frame(row: Row) -> None:
    device = Recorder()
    m = asleep(row, {"USART1": [device]})
    transmit(m, row, 0x41)
    asyncio.run(m.run(for_=us(5)))
    at = m.time
    m.periph.RCC.APB2RSTR.USART1RST = 1
    m.periph.RCC.APB2RSTR.USART1RST = 0
    asyncio.run(m.run(for_=ms(1)))
    assert device.errors == [(UartError.ABORTED, at.ps)]
    assert device.frames == []


def test_a_frame_cut_by_clearing_ue_is_an_abort_where_ue_cuts_it(row: Row) -> None:
    device = Recorder()
    m = asleep(row, {"USART1": [device]})
    transmit(m, row, 0x41)
    asyncio.run(m.run(for_=us(5)))
    at = m.time
    m.periph.USART1[row.enable if row.enable == "CR1" else "CR1_enabled"].UE = 0
    asyncio.run(m.run(for_=ms(1)))
    if row.status == "SR":
        assert device.errors == []
        assert [byte for byte, _ in device.frames] == [0x41]
    else:
        assert device.errors == [(UartError.ABORTED, at.ps)]
        assert device.frames == []


def test_a_write_refuses_the_abort_tag(u575: popcorn.PartDescription) -> None:
    m = popcorn.Machine(u575)
    with pytest.raises(ValueError, match="ABORTED"):
        m.periph.USART1.port.write(b"x", error=UartError.ABORTED)


def test_a_hook_exception_stops_the_run_naming_the_device(u575: popcorn.PartDescription) -> None:
    class Broken(Starter):
        def on_frame(self, byte: int, at: VirtualTime) -> None:
            raise LookupError("nothing here")

    device = Broken(b"x")
    m = popcorn.Machine(u575, image=popcorn.Image.elf(BOARD_ELF), devices={"USART1": [device]})
    m.reset()
    asyncio.run(m.run(until=m.sym("ready"), budget=1_000_000))
    with pytest.raises(
        popcorn.ModelError, match=r"Broken on USART1.*on_frame.*nothing here"
    ) as raised:
        asyncio.run(m.run(for_=ms(5)))
    assert raised.value.peripheral == "USART1"
    assert isinstance(raised.value.__cause__, LookupError)
    assert asyncio.run(m.run(for_=us(10))).reason is popcorn.StopReason.TIME


@pytest.mark.parametrize(
    ("devices", "reason"),
    [
        (lambda: {"I2C1": [Recorder()]}, "a UART peripheral for a UartDevice"),
        (lambda: {"USART1": [Recorder(), Recorder()]}, "one UartDevice alone on USART1"),
        (lambda: {"RCC": [Recorder()]}, "an I2C, SPI or UART peripheral"),
    ],
)
def test_refused_attachments(u575: popcorn.PartDescription, devices: object, reason: str) -> None:
    with pytest.raises(popcorn.ConfigError, match=reason):
        popcorn.Machine(u575, devices=devices())  # pyright: ignore


def test_a_stream_and_a_pty_refuse_a_uart_with_a_device(u575: popcorn.PartDescription) -> None:
    m = popcorn.Machine(u575, devices={"USART1": [Recorder()]})
    with pytest.raises(popcorn.ConfigError, match="USART1 has a UartDevice"):
        _ = m.periph.USART1.port
    with pytest.raises(popcorn.ConfigError, match="USART1 has a UartDevice"):
        m.board.pty("USART1")
    assert m.periph.USART2.port.available == 0


def test_a_described_pty_refuses_a_uart_with_a_device() -> None:
    description = popcorn.MachineDescription(
        part="STM32U575QGIx",
        images=[popcorn.Elf(BOARD_ELF)],
        board={
            "USART1": popcorn.Periph(devices=[popcorn.Device(Recorder)], connect="pty"),
        },
    )
    with pytest.raises(popcorn.ConfigError, match="USART1 has a UartDevice"):
        description.machine()


def test_a_link_refuses_a_uart_with_a_device(u575: popcorn.PartDescription) -> None:
    a = popcorn.Machine(u575, name="a", devices={"USART1": [Recorder()]})
    b = popcorn.Machine(u575, name="b")
    with pytest.raises(popcorn.ConfigError, match="USART1 has a UartDevice"):
        System({"a": a, "b": b}, [UartLink("a.USART1", "b.USART1", latency=us(20), baud=115_200)])


def test_a_send_is_refused_before_attach_and_off_the_machine_thread(
    u575: popcorn.PartDescription,
) -> None:
    device = Recorder()
    with pytest.raises(popcorn.ConfigError, match="serves no live machine"):
        device.send(b"x")
    m = popcorn.Machine(u575, devices={"USART1": [device]})
    m.reset()
    with pytest.raises(RuntimeError, match="machine's thread"):
        device.send(b"x")
    with pytest.raises(TypeError, match="takes bytes"):
        device.send("x")  # pyright: ignore[reportArgumentType]
    with pytest.raises(TypeError, match="VirtualTime"):
        device.send(b"x", at=5)  # pyright: ignore[reportArgumentType]

    async def running() -> None:
        async with m.running():
            with pytest.raises(RuntimeError, match="machine's thread"):
                device.send(b"x")

    m.mem.write(RAM, WFI_LOOP)
    m.reg[Reg.PC] = RAM
    m.reg[Reg.XPSR] = THUMB
    m.reg[Reg.PRIMASK] = 1
    asyncio.run(running())


class SendingFromReset(Recorder):
    def __init__(self) -> None:
        super().__init__()
        self.caught: list[tuple[str, bool]] = []

    def reset(self, kind: popcorn.ResetKind) -> None:
        super().reset(kind)
        try:
            self.send(b"x")
        except RuntimeError as error:
            self.caught.append((str(error), _core.on_host_thread()))


def test_a_send_from_reset_is_refused_on_a_lone_machine(u575: popcorn.PartDescription) -> None:
    device = SendingFromReset()
    m = popcorn.Machine(u575, devices={"USART1": [device]})
    m.reset()
    m.reset(popcorn.ResetKind.SOFTWARE)
    assert [on_host for _, on_host in device.caught] == [False, False]
    assert all("inside reset" in text for text, _ in device.caught)


def test_a_send_from_reset_is_refused_on_a_member_the_firmware_resets(
    u575: popcorn.PartDescription,
) -> None:
    device = SendingFromReset()
    a = popcorn.Machine(
        u575, image=popcorn.Image.elf(BOARD_ELF), name="a", devices={"USART1": [device]}
    )
    b = popcorn.Machine(u575, image=popcorn.Image.elf(BOARD_ELF), name="b")
    a.reset()
    b.reset()
    device.caught.clear()
    device.resets.clear()
    a.mem.write(RAM, AIRCR_RESET)
    a.reg[Reg.PC] = RAM
    a.reg[Reg.XPSR] = THUMB
    links = [UartLink("a.USART2", "b.USART2", latency=us(20), baud=115_200)]
    with System({"a": a, "b": b}, links) as system:
        asyncio.run(system.run(for_=us(200)))
    assert device.resets == [popcorn.ResetKind.SOFTWARE]
    ((text, on_host),) = device.caught
    assert on_host
    assert "inside reset" in text


def test_a_send_before_now_raises_and_queues_nothing(u575: popcorn.PartDescription) -> None:
    caught: list[BaseException] = []

    class Late(Recorder):
        def on_frame(self, byte: int, at: VirtualTime) -> None:
            try:
                self.send(b"x", at=VirtualTime(at.ps - 1))
            except ValueError as error:
                caught.append(error)
            super().on_frame(byte, at)

    device = Late()
    row = Row("STM32U575QGIx", "CR1_disabled", "TDR", "ISR_enabled", "RXFNE", "RDR", u575)
    m = asleep(row, {"USART1": [device]})
    transmit(m, row, 0x41)
    asyncio.run(m.run(for_=ms(5)))
    assert len(caught) == 1
    assert "lies before now" in str(caught[0])
    assert not received(m, row)


def test_a_device_keeps_no_machine_alive_and_serves_another(u575: popcorn.PartDescription) -> None:
    device = Replier(us(10))
    m = popcorn.Machine(u575, image=popcorn.Image.elf(BOARD_ELF), devices={"USART1": [device]})
    m.reset()
    asyncio.run(m.run(for_=us(100)))
    watch = weakref.ref(m)
    device_watch = weakref.ref(device)
    del m
    assert collected(watch)
    with pytest.raises(popcorn.ConfigError, match="serves no live machine"):
        device.send(b"x")
    again = popcorn.Machine(u575, image=popcorn.Image.elf(BOARD_ELF), devices={"USART1": [device]})
    again.reset()
    assert asyncio.run(again.run(for_=us(10))).reason is popcorn.StopReason.TIME
    assert len(device.attaches) == 2
    del again, device
    assert collected(device_watch)


class RaisingOnce(Recorder):
    """Raises from the hook named `hook` the first time it runs."""

    def __init__(self, hook: str) -> None:
        super().__init__()
        self.hook = hook

    def _raise(self, hook: str) -> None:
        if self.hook == hook:
            self.hook = ""
            raise LookupError(hook)

    def on_frame(self, byte: int, at: VirtualTime) -> None:
        super().on_frame(byte, at)
        self._raise("on_frame")

    def on_error(self, kind: UartError, at: VirtualTime) -> None:
        super().on_error(kind, at)
        self._raise("on_error")

    def reset(self, kind: popcorn.ResetKind) -> None:
        super().reset(kind)
        self._raise("reset")


def test_a_raising_on_frame_leaves_the_transmitter_running(row: Row) -> None:
    device = RaisingOnce("on_frame")
    m = asleep(row, {"USART1": [device]})
    transmit(m, row, 0x41)
    transmit(m, row, 0x42)
    with pytest.raises(popcorn.ModelError, match="on_frame"):
        asyncio.run(m.run(for_=ms(5)))
    asyncio.run(m.run(for_=ms(5)))
    assert [byte for byte, _ in device.frames] == [0x41, 0x42]
    status = m.periph.USART1[row.status]
    assert status.TC == 1


def test_a_raising_on_error_from_a_ue_clear_still_cuts_the_frame(row: Row) -> None:
    if row.status == "SR":
        pytest.skip("UE lets the frame finish on this row, RM0038 27.6.4")
    device = RaisingOnce("on_error")
    m = asleep(row, {"USART1": [device]})
    transmit(m, row, 0x41)
    asyncio.run(m.run(for_=us(5)))
    with pytest.raises(popcorn.ModelError, match="on_error"):
        m.periph.USART1[row.enable if row.enable == "CR1" else "CR1_enabled"].UE = 0
    asyncio.run(m.run(for_=ms(1)))
    assert len(device.errors) == 1
    assert device.frames == []


def test_a_raising_on_error_from_the_reset_bit_still_resets_the_uart(row: Row) -> None:
    device = RaisingOnce("on_error")
    m = asleep(row, {"USART1": [device]})
    transmit(m, row, 0x41)
    asyncio.run(m.run(for_=us(5)))
    with pytest.raises(popcorn.ModelError, match="on_error"):
        m.periph.RCC.APB2RSTR.USART1RST = 1
    m.periph.RCC.APB2RSTR.USART1RST = 0
    assert m.periph.USART1.BRR.value == 0
    asyncio.run(m.run(for_=ms(1)))
    assert device.frames == []


def test_a_raising_reset_hook_still_resets_every_other_model(row: Row) -> None:
    raising = RaisingOnce("")
    other = asleep(row, {"USART1": [raising]})
    asyncio.run(other.run(for_=us(100)))
    other.periph.USART2.BRR.value = 77
    raising.hook = "reset"
    with pytest.raises(popcorn.ModelError, match="reset"):
        other.reset(popcorn.ResetKind.SOFTWARE)
    assert other.periph.USART1.BRR.value == 0
    assert other.periph.USART2.BRR.value == 0
    assert other.time.ps == 0
    arm(other, row)
    transmit(other, row, 0x41)
    asyncio.run(other.run(for_=ms(5)))
    assert [byte for byte, _ in raising.frames] == [0x41]


def test_a_machine_stopped_by_a_hook_exception_is_collected(u575: popcorn.PartDescription) -> None:
    class Broken(Starter):
        def on_frame(self, byte: int, at: VirtualTime) -> None:
            raise LookupError("gone")

    m = popcorn.Machine(
        u575, image=popcorn.Image.elf(BOARD_ELF), devices={"USART1": [Broken(b"x")]}
    )
    m.reset()
    asyncio.run(m.run(until=m.sym("ready"), budget=1_000_000))
    with pytest.raises(popcorn.ModelError):
        asyncio.run(m.run(for_=ms(5)))
    watch = weakref.ref(m)
    del m
    assert collected(watch)


def echoed(u575: popcorn.PartDescription, *, running: bool) -> list[tuple[int, int]]:
    device = KickedReplier(us(100))
    m = popcorn.Machine(u575, image=popcorn.Image.elf(BOARD_ELF), devices={"USART1": [device]})
    m.reset()

    async def body() -> None:
        await m.run(until=m.sym("ready"), budget=1_000_000)
        if running:
            async with m.running():
                await m.elapse(ms(10))
        else:
            await m.run(for_=ms(10))

    asyncio.run(body())
    return device.frames


def test_two_runs_and_parked_or_running_give_the_same_stamps(
    u575: popcorn.PartDescription,
) -> None:
    first = echoed(u575, running=False)
    assert len(first) > 20
    assert echoed(u575, running=False) == first
    running = echoed(u575, running=True)
    assert running[: len(first) - 1] == first[: len(first) - 1]


def test_a_device_on_a_member_keeps_its_stamps_at_any_quantum(
    u575: popcorn.PartDescription,
) -> None:
    def run(third: int | None) -> list[tuple[int, int]]:
        device = KickedReplier(us(100))
        a = popcorn.Machine(
            u575, image=popcorn.Image.elf(BOARD_ELF), name="a", devices={"USART1": [device]}
        )
        b = popcorn.Machine(u575, image=popcorn.Image.elf(BOARD_ELF), name="b")
        a.reset()
        b.reset()
        links = [UartLink("a.USART2", "b.USART2", latency=us(third or 20), baud=115_200)]
        with System({"a": a, "b": b}, links) as system:
            asyncio.run(system.run(for_=ms(5)))
        return device.frames

    frames = run(None)
    assert len(frames) > 5
    assert run(5) == frames
    assert run(1) == frames


class Described(UartDevice):
    def __init__(self, *, marker: int) -> None:
        super().__init__()
        self.marker = marker


def test_a_machine_description_builds_a_device_on_a_uart() -> None:
    description = popcorn.MachineDescription(
        part="STM32U575QGIx",
        images=[popcorn.Elf(BOARD_ELF)],
        board={"USART1": popcorn.Periph(devices=[popcorn.Device(Described, marker=3)])},
    )
    m = description.machine()
    device = m._devices["USART1"][0]  # pyright: ignore[reportPrivateUsage]
    assert isinstance(device, Described)
    assert device.marker == 3
    bad = popcorn.MachineDescription(
        part="STM32U575QGIx",
        images=[popcorn.Elf(BOARD_ELF)],
        board={"I2C1": popcorn.Periph(devices=[popcorn.Device(Described, marker=3)])},
    )
    with pytest.raises(popcorn.ConfigError, match="a UART of"):
        bad.machine()


def test_the_host_thread_runs_a_send_from_on_attach(u575: popcorn.PartDescription) -> None:
    on_host: list[bool] = []

    class Where(Recorder):
        def on_attach(self) -> None:
            on_host.append(_core.on_host_thread())

    m = popcorn.Machine(u575, image=popcorn.Image.elf(BOARD_ELF), devices={"USART1": [Where()]})
    m.reset()
    asyncio.run(m.run(for_=us(10)))
    assert on_host == [True]
