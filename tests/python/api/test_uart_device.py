"""A device on the far end of a UART: frames on the machine's thread at their stamps, sends at the
firmware's baud, on every USART row."""

from __future__ import annotations

import asyncio
import itertools
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
    assert len(first.attaches) == 2


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
    asyncio.run(m.run(for_=us(1)))
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
        asyncio.run(m.run(for_=us(5)))
    assert threads == [True]
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
    m.periph.USART1[row.enable if row.enable == "CR1" else "CR1_enabled"].UE = 0
    with pytest.raises(popcorn.ModelError, match="on_error"):
        asyncio.run(m.run(for_=us(1)))
    asyncio.run(m.run(for_=ms(1)))
    assert len(device.errors) == 1
    assert device.frames == []


def test_a_raising_on_error_from_the_reset_bit_still_resets_the_uart(row: Row) -> None:
    device = RaisingOnce("on_error")
    m = asleep(row, {"USART1": [device]})
    transmit(m, row, 0x41)
    asyncio.run(m.run(for_=us(5)))
    m.periph.RCC.APB2RSTR.USART1RST = 1
    m.periph.RCC.APB2RSTR.USART1RST = 0
    assert m.periph.USART1.BRR.value == 0
    with pytest.raises(popcorn.ModelError, match="on_error"):
        asyncio.run(m.run(for_=us(1)))
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
    with pytest.raises(RuntimeError, match="machine's thread"):
        raising.send(b"x")
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


class FlowRecorder(Recorder):
    def __init__(self) -> None:
        super().__init__()
        self.edges: list[tuple[bool, int]] = []
        self.breaks: list[int] = []
        self.edge_threads: list[int] = []

    def on_rts(self, ready: bool, at: VirtualTime) -> None:
        self.edges.append((ready, at.ps))
        self.edge_threads.append(threading.get_ident())

    def on_break(self, at: VirtualTime) -> None:
        self.breaks.append(at.ps)


class Opening(FlowRecorder):
    """Runs `opening(self)` when attached."""

    def __init__(self, opening: Callable[[FlowRecorder], None]) -> None:
        super().__init__()
        self.opening = opening

    def on_attach(self) -> None:
        super().on_attach()
        self.opening(self)


def flag(m: popcorn.Machine, row: Row, name: str) -> bool:
    return getattr(m.periph.USART1[row.status], name) != 0


def flush(m: popcorn.Machine, row: Row) -> None:
    """Empty the receiver through a register write, which frees it as a read does."""
    uart = m.periph.USART1
    if row.status == "SR":
        uart.SR.RXNE = 0
    else:
        uart.RQR.RXFRQ = 1


def hold_cts_for_two_ms(device: FlowRecorder) -> None:
    device.drive_cts(False, at=device.now + us(1))
    device.drive_cts(True, at=device.now + ms(2))


def test_cts_holds_the_next_frame_and_releases_it_at_its_stamp(row: Row) -> None:
    frame_ps = frame_time(row)
    device = Opening(hold_cts_for_two_ms)
    m = asleep(row, {"USART1": [device]})
    m.periph.USART1.CR3.CTSE = 1
    transmit(m, row, 0x41)
    transmit(m, row, 0x42)
    asyncio.run(m.run(for_=ms(2) + ps(frame_ps * 2)))
    assert device.frames == [(0x41, frame_ps), (0x42, ms(2).ps + frame_ps)]


def test_a_cts_release_from_a_hook_counts_from_the_stamp_past_a_block_lag(row: Row) -> None:
    class Releasing(FlowRecorder):
        def on_attach(self) -> None:
            super().on_attach()
            self.drive_cts(False, at=self.now + us(1))

        def on_frame(self, byte: int, at: VirtualTime) -> None:
            super().on_frame(byte, at)
            late.append(m.time.ps - at.ps)
            self.drive_cts(True)

    late: list[int] = []
    frame_ps = frame_time(row)
    device = Releasing()
    m = asleep(row, {"USART1": [device]})
    m.mem.write(RAM, NOP * 40 + BRANCH_BACK_40_NOPS)
    m.periph.USART1.CR3.CTSE = 1
    transmit(m, row, 0x41)
    transmit(m, row, 0x42)
    asyncio.run(m.run(for_=ps(frame_ps * 3)))
    assert late[0] > 0
    first = device.frames[0][1]
    assert device.frames[1] == (0x42, first + frame_ps)


def test_drive_cts_and_send_break_are_refused_as_send_is(u575: popcorn.PartDescription) -> None:
    device = FlowRecorder()
    with pytest.raises(popcorn.ConfigError, match="serves no live machine"):
        device.drive_cts(True)
    with pytest.raises(popcorn.ConfigError, match="serves no live machine"):
        device.send_break()
    m = popcorn.Machine(u575, devices={"USART1": [device]})
    m.reset()
    with pytest.raises(RuntimeError, match=r"drive_cts\(\) runs on the machine's thread"):
        device.drive_cts(True)
    with pytest.raises(RuntimeError, match=r"send_break\(\) runs on the machine's thread"):
        device.send_break()
    with pytest.raises(TypeError, match="ready as a bool"):
        device.drive_cts(1)  # pyright: ignore[reportArgumentType]
    with pytest.raises(TypeError, match="obey_rts as a bool"):
        device.send(b"x", obey_rts=1)  # pyright: ignore[reportArgumentType]


def test_a_uart_without_flow_control_refuses_drive_cts_and_reads_rts_ready() -> None:
    from popcorn.stm32 import mcu
    from popcorn.stm32.description import DescriptionError

    caught: list[popcorn.ConfigError] = []

    class NoFlow(FlowRecorder):
        def on_attach(self) -> None:
            try:
                self.drive_cts(False)
            except popcorn.ConfigError as error:
                caught.append(error)

    try:
        description = mcu("STM32L152RETx")
    except DescriptionError as error:
        pytest.skip(f"no STM32L152RETx description: {error}")
    device = NoFlow()
    m = popcorn.Machine(description, devices={"UART4": [device]})
    m.reset()
    m.mem.write(RAM, WFI_LOOP)
    m.reg[Reg.PC] = RAM
    m.reg[Reg.SP] = RAM + 0x800
    m.reg[Reg.XPSR] = THUMB
    m.reg[Reg.PRIMASK] = 1
    asyncio.run(m.run(for_=us(10)))
    assert "no CTS" in str(caught[0])
    assert device.rts is True


def test_rts_edges_reach_on_rts_at_their_stamps(row: Row) -> None:
    def two(device: FlowRecorder) -> None:
        device.send(b"\x41\x42", at=device.now + us(1))

    frame_ps = frame_time(row)
    device = Opening(two)
    m = asleep(row, {"USART1": [device]}, rtse=True)
    assert device.rts is True
    start = us(1).ps
    asyncio.run(m.run(for_=ps(start + frame_ps * 3)))
    assert device.edges == [(False, start + frame_ps)]
    assert device.rts is False
    flush(m, row)
    released = m.time.ps
    assert device.rts is True
    asyncio.run(m.run(for_=ps(frame_ps * 2)))
    assert device.edges == [
        (False, start + frame_ps),
        (True, released),
        (False, released + frame_ps),
    ]
    assert threading.get_ident() not in device.edge_threads


def test_a_falling_rts_edge_keeps_the_frame_end_past_a_block_lag(row: Row) -> None:
    def one(device: FlowRecorder) -> None:
        device.send(b"\x41", at=device.now + us(1))

    frame_ps = frame_time(row)
    device = Opening(one)
    m = asleep(row, {"USART1": [device]}, rtse=True)
    m.mem.write(RAM, NOP * 40 + BRANCH_BACK_40_NOPS)
    asyncio.run(m.run(for_=ps(frame_ps * 2)))
    assert m.periph.USART1[row.rx].value & 0xFF == 0x41
    assert device.edges == [(False, us(1).ps + frame_ps)]


def test_rts_edges_cost_a_gil_entry_only_when_a_device_hears_them(
    u575: popcorn.PartDescription,
) -> None:
    class Deaf(UartDevice):
        def on_attach(self) -> None:
            self.send(b"\x41\x42\x43", at=self.now + us(1))

    row = Row("STM32U575QGIx", "CR1_disabled", "TDR", "ISR_enabled", "RXFNE", "RDR", u575)

    def entries(device: UartDevice) -> int:
        m = asleep(row, {"USART1": [device]}, rtse=True)
        asyncio.run(m.run(for_=us(10)))
        handle = m._handle  # pyright: ignore[reportPrivateUsage]
        before = handle.gil_entries()
        asyncio.run(m.run(for_=ms(2)))
        flush(m, row)
        asyncio.run(m.run(for_=ms(2)))
        return handle.gil_entries() - before

    def three(device: FlowRecorder) -> None:
        device.send(b"\x41\x42\x43", at=device.now + us(1))

    hearing = Opening(three)
    assert entries(Deaf()) == 0
    assert entries(hearing) == len(hearing.edges) == 3


def test_a_machine_reset_with_rts_held_makes_no_edge(row: Row) -> None:
    def two(device: FlowRecorder) -> None:
        device.send(b"\x41\x42", at=device.now + us(1))

    device = Opening(two)
    m = asleep(row, {"USART1": [device]}, rtse=True)
    asyncio.run(m.run(for_=ms(2)))
    assert len(device.edges) == 1
    m.reset(popcorn.ResetKind.SOFTWARE)
    arm(m, row)
    asyncio.run(m.run(for_=ms(1)))
    assert len(device.edges) == 1


def test_a_frame_ignoring_rts_overruns_a_full_receiver_at_its_stamp(row: Row) -> None:
    def overrunning(device: FlowRecorder) -> None:
        device.send(b"\x41", at=device.now + us(1))
        device.send(b"\x42\x43", at=device.now + ms(1), obey_rts=False)

    frame_ps = frame_time(row)
    step = instruction_ps(asleep(row))
    device = Opening(overrunning)
    m = asleep(row, {"USART1": [device]}, rtse=True)
    run_to(m, ms(1).ps + frame_ps - 2 * step)
    assert received(m, row)
    assert not flag(m, row, "ORE")
    run_to(m, ms(1).ps + frame_ps + 2 * step)
    assert flag(m, row, "ORE")
    assert m.periph.USART1[row.rx].value & 0xFF == 0x41


def test_a_frame_ignoring_rts_waits_behind_a_held_one(row: Row) -> None:
    def queued(device: FlowRecorder) -> None:
        device.send(b"\x41\x42", at=device.now + us(1))
        device.send(b"\x43", obey_rts=False)

    frame_ps = frame_time(row)
    step = instruction_ps(asleep(row))
    device = Opening(queued)
    m = asleep(row, {"USART1": [device]}, rtse=True)
    asyncio.run(m.run(for_=ms(5)))
    assert received(m, row)
    assert not flag(m, row, "ORE")
    flush(m, row)
    released = m.time.ps
    run_to(m, released + frame_ps + 2 * step)
    assert m.periph.USART1[row.rx].value & 0xFF == 0x42
    assert not flag(m, row, "ORE")
    run_to(m, released + 2 * frame_ps + 2 * step)
    assert flag(m, row, "ORE")
    assert m.periph.USART1[row.rx].value & 0xFF == 0x42


def test_a_device_break_reads_as_a_zero_byte_with_a_framing_error(row: Row) -> None:
    def breaking(device: FlowRecorder) -> None:
        device.send_break(at=device.now + us(50))

    frame_ps = frame_time(row)
    step = instruction_ps(asleep(row))
    m = asleep(row, {"USART1": [Opening(breaking)]})
    run_to(m, us(50).ps + frame_ps - 2 * step)
    assert not received(m, row)
    run_to(m, us(50).ps + frame_ps + 2 * step)
    assert received(m, row)
    assert flag(m, row, "FE")
    assert m.periph.USART1[row.rx].value & 0xFF == 0


def request_break(m: popcorn.Machine, row: Row) -> None:
    uart = m.periph.USART1
    if row.status == "SR":
        uart.CR1.SBK = 1
    else:
        uart.RQR.SBKRQ = 1


def test_a_firmware_break_reaches_on_break_at_the_stream_stamp(row: Row) -> None:
    stream = asleep(row)
    request_break(stream, row)
    asyncio.run(stream.run(for_=ms(5)))
    ((data, at, brk),) = [(f.data, f.at.ps, f.brk) for f in stream.periph.USART1.port.take_frames()]
    assert (data, brk) == (0, True)

    device = FlowRecorder()
    m = asleep(row, {"USART1": [device]})
    request_break(m, row)
    asyncio.run(m.run(for_=ms(5)))
    assert device.breaks == [at]
    assert device.frames == []


def test_one_device_hears_all_six_hooks(row: Row) -> None:
    class Hearing(FlowRecorder):
        def on_attach(self) -> None:
            super().on_attach()
            self.send(b"\x55", at=self.now + us(1))

    device = Hearing()
    m = asleep(row, {"USART1": [device]}, rtse=True)
    request_break(m, row)
    asyncio.run(m.run(for_=ms(5)))
    flush(m, row)
    transmit(m, row, 0x41)
    asyncio.run(m.run(for_=ms(5)))
    transmit(m, row, 0x42)
    asyncio.run(m.run(for_=us(5)))
    m.periph.RCC.APB2RSTR.USART1RST = 1
    m.periph.RCC.APB2RSTR.USART1RST = 0
    asyncio.run(m.run(for_=us(1)))
    assert device.resets == [popcorn.ResetKind.POWER_ON]
    assert len(device.attaches) == 1
    assert len(device.breaks) == 1
    assert [byte for byte, _ in device.frames] == [0x41]
    assert [kind for kind, _ in device.errors] == [UartError.ABORTED]
    assert [ready for ready, _ in device.edges][:2] == [False, True]


def read_after_a_stamp(
    row: Row, nops: int, stamp_ps: int, *, rtse: bool
) -> tuple[FlowRecorder, popcorn.Machine]:
    """A machine spinning at a branch to itself, with a frame due that fills the receiver and a
    second stamped at `stamp_ps`. At `RAM` waits a block of `nops` ending in a read of the data
    register, then a WFI loop."""

    def frames(device: FlowRecorder) -> None:
        device.send(b"\x41", at=device.now + us(1))
        device.send(b"\x42", at=VirtualTime(stamp_ps))

    base = next(p.base for p in row.description.peripherals if p.name == "USART1")
    device = Opening(frames)
    m = asleep(row, {"USART1": [device]}, rtse=rtse)
    address = base + m.periph.USART1[row.rx].offset
    branch_back = (0xE000 | (0x7FF & (-3))).to_bytes(2, "little")
    m.mem.write(RAM, NOP * nops + LDR_R0_R1 + b"\x30\xbf" + branch_back)
    m.mem.write(RAM + 0x400, BRANCH_TO_SELF)
    m.reg[Reg.PC] = RAM + 0x400
    m.reg[Reg.R1] = address
    return device, m


def test_a_stamp_past_rtse_set_late_counts_from_the_read_where_rtse_is_not_locked(
    row: Row,
) -> None:
    """CR3.RTSE locks while UE is 1 on the rows with `ISR`. On the `SR` row firmware can set it with
    a stamp already pending, which holds the frame from that write on."""
    if row.status != "SR":
        pytest.skip("RTSE is locked while UE is 1 on this row")
    frame_ps = frame_time(row)
    step = instruction_ps(asleep(row))
    nops = 40
    block = frame_ps + frame_ps // 2 + us(2).ps
    for offset in (4, nops // 2):
        stamp = block + offset * step
        device, m = read_after_a_stamp(row, nops, stamp, rtse=False)
        run_to(m, block)
        assert received(m, row)
        m.periph.USART1.CR3.RTSE = 1
        assert device.rts is False
        m.reg[Reg.PC] = RAM
        asyncio.run(m.run(for_=ps((nops + 4) * step)))
        assert m.reg[Reg.R0] & 0xFF == 0x41
        due = device.edges[-1][1] + frame_ps
        assert due > stamp + frame_ps
        run_to(m, due - 2 * step)
        assert not received(m, row)
        run_to(m, due + 2 * step)
        assert received(m, row)


def read_full_register_legs(row: Row, nops: int, *, rtse: bool) -> list[int]:
    """When the second frame lands, for a stamp before the read block, one dispatched before the
    read, and for stamps inside the block, past its start, not yet dispatched at the read."""
    frame_ps = frame_time(row)
    step = instruction_ps(asleep(row))
    block = frame_ps + frame_ps // 2 + us(2).ps
    landed: list[int] = []
    for stamp in (block - frame_ps // 4, block + 4 * step, block + (nops // 2) * step):
        device, m = read_after_a_stamp(row, nops, stamp, rtse=rtse)
        run_to(m, block)
        assert received(m, row)
        m.reg[Reg.PC] = RAM
        asyncio.run(m.run(for_=ps((nops + 4) * step)))
        assert m.reg[Reg.R0] & 0xFF == 0x41
        assert not received(m, row)
        due = device.edges[-1][1] + frame_ps if rtse else stamp + frame_ps
        run_to(m, due - 2 * step)
        assert not received(m, row), (stamp, due)
        run_to(m, due + 2 * step)
        assert received(m, row), (stamp, due)
        assert m.periph.USART1[row.rx].value & 0xFF == 0x42
        landed.append(due)
    return landed


@pytest.mark.parametrize("nops", [10, 40])
def test_a_read_of_a_full_register_leaves_a_stamp_alone_without_rtse(row: Row, nops: int) -> None:
    read_full_register_legs(row, nops, rtse=False)


@pytest.mark.parametrize("nops", [10, 40])
def test_with_rtse_a_stamp_past_a_full_receiver_counts_from_the_read(row: Row, nops: int) -> None:
    read_full_register_legs(row, nops, rtse=True)


def nop_loop(nops: int) -> bytes:
    branch = 0xE000 | ((-(2 * nops + 4)) >> 1 & 0x7FF)
    return NOP * nops + branch.to_bytes(2, "little")


class Ordered(FlowRecorder):
    """Logs each hook with its stamp and `now`, sends a frame at 50 us, holds CTS until `release`
    past it."""

    def __init__(self, release: VirtualDuration) -> None:
        super().__init__()
        self.release = release
        self.log: list[tuple[str, int, int]] = []

    def on_attach(self) -> None:
        super().on_attach()
        self.drive_cts(False, at=self.now)
        self.send(b"\x41", at=self.now + us(50))
        self.drive_cts(True, at=self.now + us(50) + self.release)

    def on_frame(self, byte: int, at: VirtualTime) -> None:
        self.log.append(("frame", at.ps, self.now.ps))

    def on_rts(self, ready: bool, at: VirtualTime) -> None:
        self.log.append(("rts", at.ps, self.now.ps))


def hook_order(row: Row, nops: int, release: int) -> list[tuple[str, int, int]]:
    device = Ordered(ps(release))
    m = asleep(row, {"USART1": [device]}, rtse=True)
    m.periph.USART1.CR3.CTSE = 1
    m.mem.write(RAM, nop_loop(nops))
    m.mem.write(RAM + 0x400, BRANCH_TO_SELF)
    m.reg[Reg.PC] = RAM + 0x400
    asyncio.run(m.run(for_=us(5)))
    transmit(m, row, 0x42)
    m.reg[Reg.PC] = RAM
    asyncio.run(m.run(for_=us(50) + ps(frame_time(row) * 3)))
    return device.log


def test_hooks_run_in_stamp_order_at_any_block_shape(row: Row) -> None:
    frame_ps = frame_time(row)
    release = 3 * instruction_ps(asleep(row))
    short = hook_order(row, 2, release)
    assert short == hook_order(row, 200, release)
    assert [name for name, _, _ in short] == ["rts", "frame"]
    assert short[0][1] == us(50).ps + frame_ps
    assert short[1][1] == us(50).ps + release + frame_ps
    assert all(at == now for _, at, now in short)


def test_a_far_cts_stamp_after_a_reset_raises(row: Row) -> None:
    caught: list[ValueError] = []

    class Far(FlowRecorder):
        def on_frame(self, byte: int, at: VirtualTime) -> None:
            try:
                self.drive_cts(True, at=VirtualTime(2**64 - 1000))
            except ValueError as error:
                caught.append(error)

    m = asleep(row, {"USART1": [Far()]})
    asyncio.run(m.run(for_=ms(1)))
    m.reset(popcorn.ResetKind.SOFTWARE)
    arm(m, row)
    transmit(m, row, 0x41)
    asyncio.run(m.run(for_=ms(5)))
    assert "64-bit picosecond range" in str(caught[0])


def test_rts_falls_at_the_eighth_frame_with_the_fifo_on(row: Row) -> None:
    if row.enable == "CR1":
        pytest.skip("this row has no RX FIFO")

    def nine(device: FlowRecorder) -> None:
        device.send(bytes(range(9)), at=device.now + us(1))

    frame_ps = frame_time(row)
    device = Opening(nine)
    m = asleep(row, {"USART1": [device]}, rtse=True)
    m.periph.USART1[row.enable].poke(FIFOEN=1)
    asyncio.run(m.run(for_=ps(us(1).ps + 10 * frame_ps)))
    assert device.edges == [(False, us(1).ps + 8 * frame_ps)]


def held_rts(device: FlowRecorder) -> None:
    device.send(b"\x41\x42", at=device.now + us(1))


def test_a_restored_machine_with_rts_held_goes_on_from_the_held_level(
    row: Row, tmp_path: Path
) -> None:
    class Overrunning(FlowRecorder):
        def on_rts(self, ready: bool, at: VirtualTime) -> None:
            super().on_rts(ready, at)
            if not ready:
                self.send(b"\x43", obey_rts=False)

    class Saving(Opening):
        def snapshot(self) -> bytes:
            return b""

        def restore(self, data: bytes) -> None:
            pass

    frame_ps = frame_time(row)
    m = asleep(row, {"USART1": [Saving(held_rts)]}, rtse=True)
    asyncio.run(m.run(for_=ms(5)))
    path = tmp_path / "held.snapshot"
    m.snapshot().save(path)
    device = Overrunning()
    other = popcorn.Machine(row.description, devices={"USART1": [device]})
    other.restore(other.load_snapshot(path))
    assert device.rts is False
    asyncio.run(other.run(for_=us(10)))
    assert device.edges == []
    flush(other, row)
    released = other.time.ps
    asyncio.run(other.run(for_=ps(3 * frame_ps)))
    assert device.edges[0] == (True, released)
    assert device.edges[1] == (False, released + frame_ps)
    assert flag(other, row, "ORE")


class RaisingFlow(FlowRecorder):
    def __init__(self, hook: str) -> None:
        super().__init__()
        self.hook = hook

    def on_rts(self, ready: bool, at: VirtualTime) -> None:
        super().on_rts(ready, at)
        if self.hook == "on_rts":
            self.hook = ""
            raise LookupError("on_rts")

    def on_break(self, at: VirtualTime) -> None:
        super().on_break(at)
        if self.hook == "on_break":
            self.hook = ""
            raise LookupError("on_break")


def test_a_raising_on_rts_is_named_and_the_run_goes_on(row: Row) -> None:
    class Holding(RaisingFlow):
        def on_attach(self) -> None:
            self.send(b"\x41\x42", at=self.now + us(1))

    device = Holding("on_rts")
    m = asleep(row, {"USART1": [device]}, rtse=True)
    with pytest.raises(popcorn.ModelError, match="on_rts"):
        asyncio.run(m.run(for_=ms(5)))
    asyncio.run(m.run(for_=us(10)))
    flush(m, row)
    asyncio.run(m.run(for_=ms(5)))
    assert [ready for ready, _ in device.edges] == [False, True, False]
    assert m.periph.USART1[row.rx].value & 0xFF == 0x42


def test_a_raising_on_break_leaves_the_transmitter_running(row: Row) -> None:
    device = RaisingFlow("on_break")
    m = asleep(row, {"USART1": [device]})
    request_break(m, row)
    transmit(m, row, 0x41)
    with pytest.raises(popcorn.ModelError, match="on_break"):
        asyncio.run(m.run(for_=ms(5)))
    asyncio.run(m.run(for_=ms(5)))
    assert [byte for byte, _ in device.frames] == [0x41]
    assert flag(m, row, "TC")


def test_a_device_break_waits_for_rts(row: Row) -> None:
    def breaking(device: FlowRecorder) -> None:
        device.send(b"\x41", at=device.now + us(1))
        device.send_break()

    frame_ps = frame_time(row)
    step = instruction_ps(asleep(row))
    m = asleep(row, {"USART1": [Opening(breaking)]}, rtse=True)
    asyncio.run(m.run(for_=ms(5)))
    assert not flag(m, row, "FE")
    assert release_times(row, m, frame_ps, step, lambda: flush(m, row)) == (False, True)
    assert flag(m, row, "FE")
    assert m.periph.USART1[row.rx].value & 0xFF == 0


def test_a_cts_change_keeps_its_board_instant_across_a_reset(row: Row) -> None:
    frame_ps = frame_time(row)
    device = Opening(hold_cts_for_two_ms)
    m = asleep(row, {"USART1": [device]})
    asyncio.run(m.run(for_=ms(1)))
    before = m.time.ps
    m.reset(popcorn.ResetKind.SOFTWARE)
    arm(m, row)
    m.periph.USART1.CR3.CTSE = 1
    transmit(m, row, 0x41)
    asyncio.run(m.run(for_=ms(4)))
    assert device.frames == [(0x41, ms(2).ps - before + frame_ps)]


def test_the_reset_bit_with_rts_held_raises_rts(row: Row) -> None:
    device = Opening(held_rts)
    m = asleep(row, {"USART1": [device]}, rtse=True)
    asyncio.run(m.run(for_=ms(5)))
    assert [ready for ready, _ in device.edges] == [False]
    m.periph.RCC.APB2RSTR.USART1RST = 1
    at = m.time.ps
    m.periph.RCC.APB2RSTR.USART1RST = 0
    asyncio.run(m.run(for_=us(10)))
    assert device.edges[1] == (True, at)


def test_the_new_hooks_on_a_member_keep_their_stamps_at_any_quantum(
    u575: popcorn.PartDescription,
) -> None:
    def run(third: int | None) -> list[tuple[str, int]]:
        log: list[tuple[str, int]] = []

        class Hearing(KickedReplier):
            def on_rts(self, ready: bool, at: VirtualTime) -> None:
                log.append(("rts", at.ps))

            def on_break(self, at: VirtualTime) -> None:
                log.append(("break", at.ps))

            def on_frame(self, byte: int, at: VirtualTime) -> None:
                log.append(("frame", at.ps))
                self.drive_cts(True, at=at + us(3))
                super().on_frame(byte, at)

        a = popcorn.Machine(
            u575,
            image=popcorn.Image.elf(BOARD_ELF),
            name="a",
            devices={"USART1": [Hearing(us(100))]},
        )
        b = popcorn.Machine(u575, image=popcorn.Image.elf(BOARD_ELF), name="b")
        for machine in (a, b):
            machine.reset()
            asyncio.run(machine.run(until=machine.sym("ready"), budget=1_000_000))
        a.periph.USART1.CR3.poke(RTSE=1)
        a.periph.USART1.RQR.SBKRQ = 1
        links = [UartLink("a.USART2", "b.USART2", latency=us(third or 20), baud=115_200)]
        with System({"a": a, "b": b}, links) as system:
            asyncio.run(system.run(for_=ms(5)))
        return log

    log = run(None)
    assert {name for name, _ in log} == {"rts", "break", "frame"}
    assert run(5) == log
    assert run(1) == log


def tapped(device: UartDevice, towards: str | None = None) -> list[popcorn.TapFrame]:
    return device.tap_frames(towards)


def test_the_tap_is_off_by_default_and_records_nothing(row: Row) -> None:
    device = Recorder()
    assert not device.tapping
    m = asleep(row, {"USART1": [device]})
    transmit(m, row, 0x41)
    asyncio.run(m.run(for_=ms(3)))
    assert len(device.frames) == 1
    assert tapped(device) == []
    assert device.tap_dropped == 0
    with pytest.raises(TypeError):
        device.tapping = 1  # type: ignore[assignment]


def test_a_firmware_frame_is_tapped_with_the_native_start_and_stop_stamps(row: Row) -> None:
    device = Recorder()
    device.tapping = True
    m = asleep(row, {"USART1": [device]})
    transmit(m, row, 0x41)
    asyncio.run(m.run(for_=ms(3)))
    ((data, end_ps),) = device.frames
    (frame,) = tapped(device)
    assert (frame.data, frame.index, frame.towards) == (data, 0, "device")
    assert frame.end.ps == end_ps == frame.arrival.ps
    assert frame.end.ps - frame.start.ps == frame_time(row)
    assert not (frame.brk or frame.lost or frame.injected or frame.dropped or frame.cut)
    assert tapped(device) == []


def test_a_reply_is_tapped_back_to_back_after_the_frame_it_answers(row: Row) -> None:
    device = Replier(us(100))
    device.tapping = True
    m = asleep(row, {"USART1": [device]})
    transmit(m, row, 0x41)
    asyncio.run(m.run(for_=ms(5)))
    (sent,) = tapped(device, "device")
    (reply,) = tapped(device, "USART1")
    assert (reply.data, reply.index, reply.towards) == (0xBE, 0, "USART1")
    assert reply.start.ps == sent.end.ps + us(100).ps
    assert reply.end.ps - reply.start.ps == frame_time(row)
    assert received(m, row)
    assert tapped(device) == []


def test_a_burst_from_the_device_is_tapped_in_order_with_the_gaps_the_uart_gave(row: Row) -> None:
    device = Starter(b"\x11\x22\x33", 200)
    device.tapping = True
    m = asleep(row, {"USART1": [device]})
    asyncio.run(m.run(for_=ms(5)))
    frames = tapped(device)
    assert [frame.data for frame in frames] == [0x11, 0x22, 0x33]
    assert [frame.index for frame in frames] == [0, 1, 2]
    assert frames[0].start.ps >= us(200).ps
    for before, after in itertools.pairwise(frames):
        assert after.start.ps == before.end.ps
    assert all(frame.end.ps - frame.start.ps == frame_time(row) for frame in frames)


def test_both_directions_come_ordered_by_their_end_and_a_direction_alone_is_filtered(
    row: Row,
) -> None:
    device = Replier(us(100))
    device.tapping = True
    m = asleep(row, {"USART1": [device]})
    transmit(m, row, 0x41)
    asyncio.run(m.run(for_=ms(5)))
    both = tapped(device)
    assert [frame.towards for frame in both] == ["device", "USART1"]
    assert both[0].end.ps < both[1].end.ps
    with pytest.raises(ValueError, match="neither"):
        device.tap_frames("USART2")
    with pytest.raises(TypeError):
        device.tap_frames(5)  # type: ignore[arg-type]


class Breaking(Recorder):
    def on_attach(self) -> None:
        super().on_attach()
        self.send_break(at=self.now + us(20))


def test_a_break_from_the_device_is_tapped_as_a_break(row: Row) -> None:
    device = Breaking()
    device.tapping = True
    m = asleep(row, {"USART1": [device]})
    asyncio.run(m.run(for_=ms(5)))
    (frame,) = tapped(device)
    assert frame.brk and frame.data == 0 and frame.towards == "USART1"
    assert frame.start.ps >= us(20).ps


def test_a_frame_aborted_by_the_uart_reset_bit_is_tapped_lost_at_the_abort(row: Row) -> None:
    device = Recorder()
    device.tapping = True
    m = asleep(row, {"USART1": [device]})
    transmit(m, row, 0x41)
    asyncio.run(m.run(for_=us(5)))
    at = m.time
    m.periph.RCC.APB2RSTR.USART1RST = 1
    m.periph.RCC.APB2RSTR.USART1RST = 0
    asyncio.run(m.run(for_=ms(1)))
    (frame,) = tapped(device)
    assert frame.lost and frame.data == 0x41 and frame.end.ps == at.ps
    assert frame.start.ps < frame.end.ps


def test_tapped_frames_stay_across_a_machine_reset_and_their_index_goes_on(row: Row) -> None:
    device = Recorder()
    device.tapping = True
    m = asleep(row, {"USART1": [device]})
    transmit(m, row, 0x41)
    asyncio.run(m.run(for_=ms(3)))
    m.reset()
    arm(m, row)
    transmit(m, row, 0x42)
    asyncio.run(m.run(for_=ms(3)))
    first, second = tapped(device)
    assert (first.data, first.index) == (0x41, 0)
    assert (second.data, second.index) == (0x42, 1)
    assert device.tapping
    assert second.end.ps == first.end.ps


def test_a_restore_drops_the_tapped_frames_and_restarts_index_and_dropped(row: Row) -> None:
    device = Recorder()
    device.tapping = True
    m = asleep(row, {"USART1": [device]})
    saved = m.snapshot()
    transmit(m, row, 0x41)
    asyncio.run(m.run(for_=ms(3)))
    assert len(device.frames) == 1
    m.restore(saved)
    assert tapped(device) == []
    assert device.tap_dropped == 0
    assert device.tapping
    arm(m, row)
    transmit(m, row, 0x42)
    asyncio.run(m.run(for_=ms(3)))
    (frame,) = tapped(device)
    assert (frame.data, frame.index) == (0x42, 0)


def test_the_tap_keeps_a_bounded_number_and_counts_the_oldest_it_dropped(
    u575: popcorn.PartDescription, monkeypatch: pytest.MonkeyPatch
) -> None:
    from popcorn import _devices

    monkeypatch.setattr(_devices, "_TAP_KEEP", 3)
    device = Starter(b"abcde", 100)
    device.tapping = True
    row = Row("STM32U575QGIx", "CR1_disabled", "TDR", "ISR_enabled", "RXFNE", "RDR", u575)
    m = asleep(row, {"USART1": [device]})
    asyncio.run(m.run(for_=ms(10)))
    frames = tapped(device)
    assert [frame.data for frame in frames] == [ord(c) for c in "cde"]
    assert [frame.index for frame in frames] == [2, 3, 4]
    assert device.tap_dropped == 2


def test_a_tapped_device_costs_one_gil_entry_per_frame_it_is_sent_and_none_untapped(
    u575: popcorn.PartDescription,
) -> None:
    def entries(tap: bool) -> int:
        device = Starter(b"abcdefgh")
        device.tapping = tap
        m = popcorn.Machine(u575, image=popcorn.Image.elf(BOARD_ELF), devices={"USART1": [device]})
        m.reset()
        asyncio.run(m.run(until=m.sym("ready"), budget=1_000_000))
        before = m._handle.gil_entries()  # pyright: ignore[reportPrivateUsage]
        asyncio.run(m.run(for_=ms(20)))
        return m._handle.gil_entries() - before  # pyright: ignore[reportPrivateUsage]

    assert entries(False) == 8
    assert entries(True) == 16


class Everything(Recorder):
    def __init__(self) -> None:
        super().__init__()
        self.heard: set[str] = set()

    def on_attach(self) -> None:
        self.heard.add("attach")
        self.send(b"\x11", at=self.now)

    def on_frame(self, byte: int, at: VirtualTime) -> None:
        self.heard.add("frame")

    def on_break(self, at: VirtualTime) -> None:
        self.heard.add("break")

    def on_rts(self, ready: bool, at: VirtualTime) -> None:
        self.heard.add("rts")

    def on_error(self, kind: UartError, at: VirtualTime) -> None:
        self.heard.add("error")

    def reset(self, kind: popcorn.ResetKind) -> None:
        self.heard.add("reset")

    def snapshot(self) -> bytes:
        self.heard.add("save")
        return b""

    def restore(self, data: bytes) -> None:
        self.heard.add("load")


def test_one_tapped_device_runs_every_native_hook_in_one_life(
    u575: popcorn.PartDescription,
) -> None:
    row = Row("STM32U575QGIx", "CR1_disabled", "TDR", "ISR_enabled", "RXFNE", "RDR", u575)
    device = Everything()
    device.tapping = True
    m = asleep(row, {"USART1": [device]}, rtse=True)
    transmit(m, row, 0x41)
    asyncio.run(m.run(for_=ms(2)))
    m.periph.USART1.RQR.SBKRQ = 1
    asyncio.run(m.run(for_=ms(2)))
    transmit(m, row, 0x42)
    asyncio.run(m.run(for_=us(5)))
    m.periph.RCC.APB2RSTR.USART1RST = 1
    m.periph.RCC.APB2RSTR.USART1RST = 0
    asyncio.run(m.run(for_=ms(1)))
    kinds = {(frame.towards, frame.brk, frame.lost) for frame in device.tap_frames()}
    assert kinds == {
        ("device", False, False),
        ("device", True, False),
        ("device", False, True),
        ("USART1", False, False),
    }
    saved = m.snapshot()
    m.restore(saved)
    asyncio.run(m.run(for_=us(10)))
    assert device.heard == {
        "attach",
        "frame",
        "break",
        "rts",
        "error",
        "reset",
        "save",
        "load",
    }


class SwitchOn(Recorder):
    def on_frame(self, byte: int, at: VirtualTime) -> None:
        super().on_frame(byte, at)
        self.tapping = True


def test_tapping_set_from_a_thread_or_from_a_hook_taps_the_frames_after_it(row: Row) -> None:
    device = SwitchOn()
    m = asleep(row, {"USART1": [device]})
    transmit(m, row, 0x41)
    asyncio.run(m.run(for_=ms(3)))
    assert tapped(device) == []
    transmit(m, row, 0x42)
    asyncio.run(m.run(for_=ms(3)))
    assert [frame.data for frame in tapped(device)] == [0x42]
    device.tapping = False
    thread = threading.Thread(target=setattr, args=(device, "tapping", True))
    thread.start()
    thread.join()
    assert device.tapping
    transmit(m, row, 0x43)
    asyncio.run(m.run(for_=ms(3)))
    assert [frame.data for frame in tapped(device)] == [0x43]


class SeesTraffic(Starter):
    def __init__(self, data: bytes) -> None:
        super().__init__(data)
        self.flags: list[bool] = []

    def _received(self, data: int, start: int, end: int, brk: bool) -> None:
        self.flags.append(self._traffic)
        super()._received(data, start, end, brk)
        self.flags.append(self._traffic)


def test_a_received_frame_finds_the_traffic_flag_already_set(row: Row) -> None:
    device = SeesTraffic(b"ab")
    device.tapping = True
    m = asleep(row, {"USART1": [device]})
    asyncio.run(m.run(for_=ms(5)))
    assert len(tapped(device)) == 2
    assert device.flags == [True] * 4


def test_a_frame_in_flight_at_a_snapshot_keeps_its_start_after_a_restore(
    row: Row, tmp_path: Path
) -> None:
    ft = frame_time(row)
    device = Recorder()
    device.tapping = True
    m = asleep(row, {"USART1": [device]})
    asyncio.run(m.run(for_=us(100)))
    transmit(m, row, 0x41)
    asyncio.run(m.run(for_=ps(ft // 2)))
    saved = m.snapshot()
    path = tmp_path / "mid.snap"
    saved.save(path)
    asyncio.run(m.run(for_=ms(2)))
    went_on = tapped(device)
    assert len(went_on) == 1
    assert went_on[0].end.ps - went_on[0].start.ps == ft
    transmit(m, row, 0x42)
    asyncio.run(m.run(for_=ms(2)))
    tapped(device)
    m.restore(saved)
    asyncio.run(m.run(for_=ms(2)))
    assert tapped(device) == went_on
    fresh = Recorder()
    fresh.tapping = True
    other = asleep(row, {"USART1": [fresh]})
    other.restore(other.load_snapshot(path))
    asyncio.run(other.run(for_=ms(2)))
    assert tapped(fresh) == went_on


@pytest.mark.parametrize("divisor", [2, 0.5])
def test_a_baud_change_inside_a_device_frame_leaves_its_start_at_the_send(
    row: Row, divisor: float
) -> None:
    ft = frame_time(row)
    device = Starter(b"\x11\x22", 100)
    device.tapping = True
    m = asleep(row, {"USART1": [device]})
    asyncio.run(m.run(for_=us(100) + ps(ft // 2)))
    m.periph.USART1.BRR.value = int(BRR / divisor)
    asyncio.run(m.run(for_=ms(3)))
    first, second = tapped(device)
    assert first.start.ps == us(100).ps
    assert first.end.ps - first.start.ps == ft
    assert second.start.ps == first.end.ps


def test_a_deviceless_snapshot_restored_into_a_tapping_device_taps_its_queued_frames(
    row: Row, tmp_path: Path
) -> None:
    source = asleep(row)
    source.periph.USART1.port.write(b"\x33\x44")
    asyncio.run(source.run(for_=us(3)))
    path = tmp_path / "queued.snap"
    source.snapshot().save(path)
    device = Recorder()
    device.tapping = True
    m = asleep(row, {"USART1": [device]})
    m.restore(m.load_snapshot(path))
    asyncio.run(m.run(for_=ms(3)))
    saved = m.snapshot()
    assert [(frame.data, frame.towards) for frame in tapped(device)] == [
        (0x33, "USART1"),
        (0x44, "USART1"),
    ]
    m.restore(saved)
    m.snapshot()


def test_a_restore_from_a_deviceless_snapshot_drops_the_tap_and_restarts_its_index(
    row: Row, tmp_path: Path
) -> None:
    source = asleep(row)
    path = tmp_path / "plain.snap"
    source.snapshot().save(path)
    device = Starter(b"\x11", 10)
    device.tapping = True
    m = asleep(row, {"USART1": [device]})
    asyncio.run(m.run(for_=ms(1)))
    m.restore(m.load_snapshot(path))
    assert tapped(device) == []
    assert device.tap_dropped == 0
    assert device._tap_index == [0, 0]  # pyright: ignore[reportPrivateUsage]


def test_a_parity_and_two_stop_bits_tap_both_directions_at_the_longer_frame(
    u575: popcorn.PartDescription,
) -> None:
    row = Row("STM32U575QGIx", "CR1_disabled", "TDR", "ISR_enabled", "RXFNE", "RDR", u575)
    device = Replier(us(100))
    device.tapping = True
    m = asleep(row, {"USART1": [device]})
    uart = m.periph.USART1
    uart.CR1_enabled.UE = 0
    uart.CR2.STOP = 2
    uart.CR1_disabled.poke(UE=1, TE=1, RE=1, PCE=1)
    transmit(m, row, 0x41)
    asyncio.run(m.run(for_=ms(5)))
    (sent,) = tapped(device, "device")
    (reply,) = tapped(device, "USART1")
    longer = frame_time(row) * 11 // 10
    assert sent.end.ps - sent.start.ps == longer
    assert reply.end.ps - reply.start.ps == longer


@pytest.mark.parametrize("divisor", [2, 0.5])
def test_a_baud_change_inside_a_firmware_frame_leaves_its_start_at_the_write(
    row: Row, divisor: float
) -> None:
    ft = frame_time(row)

    def frame(scale: float) -> popcorn.TapFrame:
        device = Recorder()
        device.tapping = True
        m = asleep(row, {"USART1": [device]})
        asyncio.run(m.run(for_=us(100)))
        transmit(m, row, 0x41)
        asyncio.run(m.run(for_=ps(ft // 2)))
        m.periph.USART1.BRR.value = int(BRR / scale)
        asyncio.run(m.run(for_=ms(3)))
        (only,) = tapped(device)
        return only

    assert frame(divisor) == frame(1)


@pytest.mark.parametrize("route", ["ue", "bus_reset"])
def test_a_nine_bit_frame_aborted_keeps_its_word_in_the_tap(
    u575: popcorn.PartDescription, route: str
) -> None:
    row = Row("STM32U575QGIx", "CR1_disabled", "TDR", "ISR_enabled", "RXFNE", "RDR", u575)
    device = Recorder()
    device.tapping = True
    m = asleep(row, {"USART1": [device]})
    uart = m.periph.USART1
    uart.CR1_enabled.UE = 0
    uart.CR1_disabled.poke(UE=1, TE=1, RE=1, M0=1)
    uart.TDR.value = 0x1A5
    asyncio.run(m.run(for_=us(5)))
    if route == "ue":
        uart.CR1_enabled.UE = 0
    else:
        m.periph.RCC.APB2RSTR.USART1RST = 1
        m.periph.RCC.APB2RSTR.USART1RST = 0
    asyncio.run(m.run(for_=ms(1)))
    (frame,) = tapped(device)
    assert frame.lost and frame.data == 0x1A5


@pytest.mark.parametrize("leg", ["same_after_a_later_frame", "fresh"])
def test_a_pending_abort_in_a_snapshot_taps_the_frame_the_run_that_went_on_tapped(
    row: Row, leg: str, tmp_path: Path
) -> None:
    def build() -> tuple[Recorder, popcorn.Machine]:
        device = Recorder()
        device.tapping = True
        m = asleep(row, {"USART1": [device]})
        uart = m.periph.USART1
        if row.enable == "CR1_disabled":
            uart[row.enable].poke(UE=0)
            uart[row.enable].poke(M0=1)
            uart[row.enable].poke(UE=1, TE=1, RE=1)
            transmit(m, row, 0x1A5)
        else:
            transmit(m, row, 0x65)
        asyncio.run(m.run(for_=us(5)))
        return device, m

    def seen(device: Recorder) -> tuple[list[popcorn.TapFrame], list[tuple[UartError, int]]]:
        return tapped(device), list(device.errors)

    device, m = build()
    m.periph.RCC.APB2RSTR.USART1RST = 1
    m.periph.RCC.APB2RSTR.USART1RST = 0
    saved = m.snapshot()
    asyncio.run(m.run(for_=ms(1)))
    went_on = seen(device)
    assert len(went_on[0]) == 1 and went_on[0][0].lost
    if leg == "fresh":
        path = tmp_path / "abort.snap"
        saved.save(path)
        other, target = build()
        tapped(other)
        other.errors.clear()
        target.restore(target.load_snapshot(path))
        asyncio.run(target.run(for_=ms(1)))
        assert seen(other) == went_on
        return
    arm(m, row)
    transmit(m, row, 0x42)
    asyncio.run(m.run(for_=ms(2)))
    device.errors.clear()
    tapped(device)
    m.restore(saved)
    asyncio.run(m.run(for_=ms(1)))
    assert seen(device) == went_on


def test_a_raising_on_frame_stops_the_run_and_the_uart_goes_on(
    u575: popcorn.PartDescription,
) -> None:
    class Raising(Recorder):
        def on_frame(self, byte: int, at: VirtualTime) -> None:
            super().on_frame(byte, at)
            if len(self.frames) == 2:
                raise LookupError("a bug in the device")

    row = Row("STM32U575QGIx", "CR1_disabled", "TDR", "ISR_enabled", "RXFNE", "RDR", u575)
    device = Raising()
    m = asleep(row, {"USART1": [device]})
    for byte in (0x41, 0x42, 0x43):
        transmit(m, row, byte)
        try:
            asyncio.run(m.run(for_=ms(2)))
        except popcorn.ModelError:
            assert byte == 0x42
    assert [frame for frame, _ in device.frames] == [0x41, 0x42, 0x43]
    assert m.periph.USART1.ISR_enabled.TC == 1
