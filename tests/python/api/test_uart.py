"""UART streams: bytes in and out of the firmware in virtual time."""

from __future__ import annotations

import asyncio
import gc
import itertools
import weakref
from pathlib import Path

import popcorn
import pytest
from popcorn import ModelError, _core
from popcorn.board import AudioOptions, Frame
from popcorn.model import Model
from popcorn.trace import TraceKind, read
from popcorn.units import VirtualTime, ms, us

from .conftest import BOARD_ELF, holds_pty, run_bounded

FRAME_PS = 87_500_000
USART1_TDR = 0x4001_3828


class Silent(Model):
    """A Python model with no register behaviour, standing in for a UART."""


async def echo(m: popcorn.Machine, data: bytes) -> tuple[VirtualTime, list[Frame]]:
    uart = m.periph.USART1.port
    start = m.time
    await uart.write(data)
    return start, await uart.read_frames(len(data), for_=ms(10))


def test_the_firmware_echoes_at_the_configured_baud(board: popcorn.Machine) -> None:
    start, frames = asyncio.run(echo(board, b"hello"))
    assert bytes(f.data for f in frames) == b"hello"
    assert frames[-1].at.ps - start.ps >= 5 * FRAME_PS
    gaps = [b.at.ps - a.at.ps for a, b in itertools.pairwise(frames)]
    assert all(gap >= FRAME_PS for gap in gaps)


def test_the_same_exchange_gives_the_same_times(u575: popcorn.MachineDescription) -> None:
    from .conftest import BOARD_ELF

    def once() -> list[tuple[int, int]]:
        m = popcorn.Machine(u575, image=popcorn.Image.elf(BOARD_ELF))
        m.reset()
        asyncio.run(m.run(until=m.sym("ready"), budget=1_000_000))
        start, frames = asyncio.run(echo(m, b"popcorn"))
        return [(f.data, f.at.ps - start.ps) for f in frames]

    assert once() == once()


def test_read_inside_running_awaits_the_background_run(board: popcorn.Machine) -> None:
    async def body() -> bytes:
        uart = board.periph.USART1.port
        async with board.running():
            await uart.write(b"hello")
            return await uart.read(5, for_=ms(10))

    assert asyncio.run(body()) == b"hello"


def test_a_read_that_times_out_returns_what_arrived(board: popcorn.Machine) -> None:
    uart = board.periph.USART1.port

    async def body() -> tuple[bytes, bytes]:
        await uart.write(b"ab")
        short = await uart.read(5, for_=us(300))
        rest = await uart.read(5, for_=us(100))
        return short, rest

    assert asyncio.run(body()) == (b"ab", b"")
    assert uart.available == 0 and uart.queued == 0


def test_queued_counts_bytes_not_yet_delivered(board: popcorn.Machine) -> None:
    uart = board.periph.USART1.port
    asyncio.run(uart.write(b"xyz"))
    assert uart.queued == 3
    assert asyncio.run(uart.read(3, for_=ms(1))) == b"xyz"
    assert uart.queued == 0


def test_the_port_is_one_stream_per_uart(board: popcorn.Machine) -> None:
    assert board.periph.USART1.port is board.periph.USART1.port


def test_a_peripheral_without_a_uart_model_has_no_port(board: popcorn.Machine) -> None:
    with pytest.raises(popcorn.ModelError) as caught:
        _ = board.periph.GPIOA.port
    assert caught.value.peripheral == "GPIOA"


def test_a_fault_while_reading_raises(board: popcorn.Machine) -> None:
    board.reg.pc = 0x6000_0000

    with pytest.raises(popcorn.EmulationError):
        asyncio.run(board.periph.USART1.port.read(1, for_=ms(1)))


def test_reads_take_frames_in_order_in_parts(board: popcorn.Machine) -> None:
    uart = board.periph.USART1.port

    async def body() -> tuple[bytes, int, bytes]:
        await uart.write(b"hello")
        await board.run(for_=ms(1))
        head = await uart.read(2)
        left = uart.available
        return head, left, await uart.read(3)

    assert asyncio.run(body()) == (b"he", 3, b"llo")


def test_a_stream_keeps_its_machine_and_both_are_freed_together(
    u575: popcorn.MachineDescription,
) -> None:
    from .conftest import BOARD_ELF

    m = popcorn.Machine(u575, image=popcorn.Image.elf(BOARD_ELF))
    uart = m.periph.USART1.port
    path = m.board.pty("USART1")
    machine = weakref.ref(m)
    del m
    gc.collect()
    assert machine() is not None and uart.queued == 0
    del uart
    gc.collect()
    assert machine() is None
    assert not holds_pty(path)


def test_frames_sent_before_the_port_is_taken_wait_for_it(board: popcorn.Machine) -> None:
    board.mem.u32[USART1_TDR] = ord("A")
    saved = board.snapshot()
    asyncio.run(board.run(for_=ms(1)))
    uart = board.periph.USART1.port
    assert uart.available == 1
    assert asyncio.run(uart.read(1)) == b"A"

    board.restore(saved)
    assert uart.available == 0
    asyncio.run(board.run(for_=ms(1)))
    assert asyncio.run(uart.read(1)) == b"A"


def test_a_secure_alias_shares_the_port_of_the_uart_it_aliases(board: popcorn.Machine) -> None:
    board.mem.u32[USART1_TDR] = ord("A")
    asyncio.run(board.run(for_=ms(1)))
    assert board.periph.SEC_USART1.port is board.periph.USART1.port
    assert asyncio.run(board.periph.SEC_USART1.port.read(1)) == b"A"

    async def echo() -> bytes:
        await board.periph.SEC_USART1.port.write(b"hi")
        return await board.periph.USART1.port.read(2, for_=ms(10))

    assert asyncio.run(echo()) == b"hi"


def test_a_python_model_in_a_uart_slot_is_not_connected(
    u575: popcorn.MachineDescription,
) -> None:
    m = popcorn.Machine(u575, models={"USART2": Silent})
    with pytest.raises(ModelError):
        _ = m.periph.USART2.port
    assert m.periph.USART1.port.available == 0


def test_a_full_queue_drops_its_oldest_frames_and_counts_them(board: popcorn.Machine) -> None:
    uart = board.periph.USART1.port
    link = board._ports.link("USART1")  # pyright: ignore[reportPrivateUsage]

    link.set_frame_limit(3)

    async def body() -> bytes:
        await uart.write(b"hello")
        await board.run(for_=ms(2))
        return await uart.read(uart.available)

    saved = board.snapshot()
    assert asyncio.run(body()) == b"llo"
    assert uart.dropped == 2
    board.restore(saved)
    assert uart.dropped == 0
    with pytest.raises(popcorn.ConfigError):
        link.set_frame_limit(0)


def test_a_restore_drops_frames_not_read_yet(board: popcorn.Machine) -> None:
    uart = board.periph.USART1.port
    saved = board.snapshot()

    async def body() -> None:
        await uart.write(b"hello")
        await board.run(for_=ms(1))

    asyncio.run(body())
    assert uart.available == 5
    board.restore(saved)
    assert uart.available == 0 and uart.queued == 0


def test_a_link_kept_past_its_handle_refuses_access(u575: popcorn.MachineDescription) -> None:
    handle = _core.MachineHandle(u575, _core.MachineConfig())
    link = handle.attach_uart("USART1")
    del handle
    gc.collect()
    with pytest.raises(popcorn.ConfigError, match="machine still exists"):
        link.send(b"x")
    with pytest.raises(popcorn.ConfigError, match="machine still exists"):
        link.queued()


def test_a_write_from_another_thread_during_a_run_is_posted(board: popcorn.Machine) -> None:
    uart = board.periph.USART1.port

    async def write() -> VirtualTime:
        return await uart.write(b"x")

    async def body() -> VirtualTime:
        async with board.running():
            return await asyncio.to_thread(asyncio.run, write())

    assert asyncio.run(body()) >= VirtualTime(0)


def test_a_read_inside_running_raises_a_handler_error(board: popcorn.Machine) -> None:
    def boom(_: popcorn.WatchHit) -> None:
        raise ValueError("echo watch failed")

    board.watch("echoed", handler=boom)
    uart = board.periph.USART1.port

    async def body() -> bytes:
        async with board.running():
            await uart.write(b"hi")
            return await uart.read(2, for_=ms(10))

    with pytest.raises(ValueError, match="echo watch failed"):
        run_bounded(body())
    assert not board.busy


def test_board_commands_run_on_the_machine_thread_without_the_gil(
    u575: popcorn.MachineDescription, tmp_path: Path
) -> None:
    m = popcorn.Machine(
        u575,
        image=popcorn.Image.elf(BOARD_ELF),
        audio={"SAI1": {"A": AudioOptions()}},
    )
    m.reset()
    asyncio.run(m.run(until=m.sym("ready"), budget=1_000_000))
    port = m.periph.USART1.port
    stream = m.periph.SAI1.audio("A")

    path = tmp_path / "t.pctr"
    tracing = m.trace([TraceKind.BLOCK], path)
    recording = tracing.__enter__()

    async def scenario() -> None:
        async with m.running():
            await port.write(b"x")
            await stream.write([1, 2])
            tracing.__exit__(None, None, None)

    asyncio.run(scenario())
    assert m._handle.gil_entries() == 0  # pyright: ignore[reportPrivateUsage]
    closed = recording.records
    assert closed > 0 and recording.size == path.stat().st_size
    assert asyncio.run(m.run(for_=us(100))).reason is not None
    assert recording.records == closed
    assert sum(1 for _ in read(path)) > 0


@pytest.mark.parametrize("running", [False, True], ids=["parked", "running"])
def test_a_uart_read_waits_on_the_machine_thread_without_the_gil(
    u575: popcorn.MachineDescription, running: bool
) -> None:
    m = popcorn.Machine(u575, image=popcorn.Image.elf(BOARD_ELF))
    m.reset()
    asyncio.run(m.run(until=m.sym("ready"), budget=1_000_000))
    port = m.periph.USART1.port

    async def scenario() -> bytes:
        await port.write(b"abc")
        if running:
            async with m.running():
                return await port.read(3, for_=ms(5))
        return await port.read(3, for_=ms(5))

    assert len(asyncio.run(scenario())) == 3
    assert m._handle.gil_entries() == 0  # pyright: ignore[reportPrivateUsage]


def test_a_trace_closed_again_after_its_posted_close_closes_once(
    u575: popcorn.MachineDescription, tmp_path: Path
) -> None:
    m = popcorn.Machine(u575, image=popcorn.Image.elf(BOARD_ELF))
    m.reset()
    asyncio.run(m.run(until=m.sym("ready"), budget=1_000_000))
    path = tmp_path / "t.pctr"
    tracing = m.trace([TraceKind.BLOCK], path)
    recording = tracing.__enter__()

    async def scenario() -> None:
        async with m.running():
            tracing.__exit__(None, None, None)
            for _ in range(100):
                try:
                    recording.close()
                    return
                except popcorn.MachineRunning:
                    await m.read()
            pytest.fail("the posted close never ran")

    asyncio.run(scenario())
    closed = recording.records
    recording.close()
    asyncio.run(m.run(for_=us(100)))
    assert recording.records == closed
    assert recording.size == path.stat().st_size
    assert all(record is not None for record in read(path))


def test_take_frames_empties_the_queue_while_running(board: popcorn.Machine) -> None:
    uart = board.periph.USART1.port

    async def body() -> tuple[int, list[Frame]]:
        async with board.running():
            await uart.write(b"hello")
            while uart.available < 5:
                await board.elapse(ms(1))
            return uart.available, uart.take_frames()

    available, frames = run_bounded(body(), 10)
    assert available == 5
    assert bytes(f.data for f in frames) == b"hello"
    assert uart.available == 0


def test_take_frames_equals_a_read_of_what_is_available(board: popcorn.Machine) -> None:
    uart = board.periph.USART1.port
    start = board.snapshot()

    async def exchanged() -> None:
        await uart.write(b"popcorn")
        await board.run(for_=ms(2))

    asyncio.run(exchanged())
    taken = uart.take_frames()
    board.restore(start)
    asyncio.run(exchanged())
    read = asyncio.run(uart.read_frames(uart.available))
    assert len(taken) == 7
    assert taken == read


def test_take_returns_nothing_from_an_empty_queue(board: popcorn.Machine) -> None:
    uart = board.periph.USART1.port
    assert uart.take() == b""
    assert uart.take_frames() == []

    async def body() -> tuple[bytes, list[Frame]]:
        async with board.running():
            return uart.take(), uart.take_frames()

    assert run_bounded(body(), 10) == (b"", [])


def test_take_gives_the_bytes_of_the_frames(board: popcorn.Machine) -> None:
    uart = board.periph.USART1.port

    async def exchanged() -> None:
        await uart.write(b"ab")
        await board.run(for_=ms(2))

    asyncio.run(exchanged())
    assert uart.take() == b"ab"
    assert uart.available == 0
