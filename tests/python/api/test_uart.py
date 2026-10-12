"""UART streams: bytes in and out of the firmware in virtual time."""

from __future__ import annotations

import asyncio
import contextlib
import gc
import itertools
import os
import re
import select
import weakref
from pathlib import Path

import popcorn
import pytest
from popcorn import ModelError, _core
from popcorn.board import AudioOptions, Frame
from popcorn.model import Model
from popcorn.trace import TraceKind, read
from popcorn.units import VirtualTime, ms, us

from .conftest import BOARD_ELF, holds_pty, late_drained_read, run_bounded, stalled

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


def test_read_frames_with_a_deadline_while_running(board: popcorn.Machine) -> None:
    uart = board.periph.USART1.port

    async def body() -> tuple[VirtualTime, list[Frame], VirtualTime]:
        async with board.running():
            start = await board.now()
            frames = await uart.read_frames(1000, deadline=start + ms(3))
            return start, frames, await board.now()

    start, frames, after = run_bounded(body(), 10)
    assert frames == []
    assert after >= start + ms(3)


def test_a_deadline_read_equals_the_same_read_with_for(board: popcorn.Machine) -> None:
    uart = board.periph.USART1.port
    start = board.snapshot()

    async def read(deadline: bool) -> tuple[list[Frame], VirtualTime]:
        await uart.write(b"abcdef")
        if deadline:
            frames = await uart.read_frames(4, deadline=board.time + us(400))
        else:
            frames = await uart.read_frames(4, for_=us(400))
        return frames, board.time

    with_deadline = asyncio.run(read(True))
    board.restore(start)
    assert asyncio.run(read(False)) == with_deadline
    assert 0 < len(with_deadline[0]) < 4


def test_a_past_deadline_read_takes_what_was_ready_by_it_at_once(board: popcorn.Machine) -> None:
    uart = board.periph.USART1.port

    async def exchanged() -> None:
        await uart.write(b"ab")
        await board.run(for_=ms(2))

    asyncio.run(exchanged())
    before = board.instructions
    assert asyncio.run(uart.read(5, deadline=VirtualTime(0))) == b""
    assert asyncio.run(uart.read(5, deadline=board.time)) == b"ab"
    assert asyncio.run(uart.read_frames(5, deadline=board.time)) == []
    assert board.instructions == before

    async def running() -> tuple[bytes, list[Frame]]:
        async with board.running():
            past = await board.now()
            return await uart.read(5, deadline=past), await uart.read_frames(1, deadline=past)

    assert run_bounded(running(), 10) == (b"", [])


@pytest.mark.parametrize("frames", [False, True], ids=["read", "read_frames"])
def test_a_uart_read_refuses_both_for_and_deadline(board: popcorn.Machine, frames: bool) -> None:
    uart = board.periph.USART1.port
    before = board.instructions
    with pytest.raises(TypeError, match="exclusive"):
        if frames:
            asyncio.run(uart.read_frames(1, for_=ms(1), deadline=board.time))
        else:
            asyncio.run(uart.read(1, for_=ms(1), deadline=board.time))
    assert board.instructions == before


def test_a_free_run_deadline_read_ends_at_its_absolute_time(board: popcorn.Machine) -> None:
    uart = board.periph.USART1.port

    async def scenario() -> tuple[VirtualTime, list[Frame]]:
        async with board.running():
            await board.elapse(ms(5))

            async def read(start: VirtualTime) -> list[Frame]:
                uart.write(b"x" * 200)
                return await uart.read_frames(1000, deadline=start + ms(3))

            return await stalled(board, read)

    start, frames = run_bounded(scenario(), 10)
    assert frames
    assert start + ms(2) < frames[-1].at <= start + ms(3) + us(200)
    assert len(frames) < 40


def test_a_free_run_deadline_read_takes_the_frames_a_parked_one_takes(
    board: popcorn.Machine,
) -> None:
    uart = board.periph.USART1.port
    sent = bytes(range(1, 201))
    start = board.snapshot()

    def post(at: VirtualTime) -> None:
        uart.write(sent, at=at + ms(1))

    async def reads(at: VirtualTime) -> tuple[list[Frame], list[Frame]]:
        first = await uart.read_frames(1000, deadline=at + ms(4))
        return first, await uart.read_frames(1000, deadline=at + ms(30))

    async def running() -> tuple[VirtualTime, tuple[list[Frame], list[Frame]]]:
        async with board.running():
            return await late_drained_read(board, post, reads, ms(8))

    at, (first, later) = run_bounded(running(), 20)
    assert first
    assert later
    assert first[-1].at <= at + ms(4) < later[0].at
    assert bytes(f.data for f in first + later) == sent
    board.restore(start)
    post(at)
    assert asyncio.run(reads(at)) == (first, later)


def test_a_read_registered_past_its_deadline_takes_only_the_frames_ready_by_it(
    board: popcorn.Machine,
) -> None:
    uart = board.periph.USART1.port
    sent = bytes(range(1, 201))
    start = board.snapshot()

    def post(at: VirtualTime) -> None:
        uart.write(sent, at=at + ms(1))

    async def reads(at: VirtualTime) -> tuple[list[Frame], list[Frame], list[Frame]]:
        waited = await uart.read_frames(1000, deadline=at + ms(2))
        fast = await uart.read_frames(30, deadline=at + ms(3))
        return waited, fast, await uart.read_frames(1000, deadline=at + ms(30))

    async def running() -> tuple[VirtualTime, tuple[list[Frame], list[Frame], list[Frame]]]:
        async with board.running():

            async def late(at: VirtualTime) -> tuple[list[Frame], list[Frame], list[Frame]]:
                await board.sample(lambda: None, at=at + ms(10))
                return await reads(at)

            return await late_drained_read(board, post, late, ms(10))

    at, (waited, fast, later) = run_bounded(running(), 20)
    assert waited
    assert waited[-1].at <= at + ms(2) < fast[0].at
    assert 0 < len(fast) < 30
    assert fast[-1].at <= at + ms(3) < later[0].at
    assert bytes(f.data for f in waited + fast + later) == sent
    board.restore(start)
    post(at)
    asyncio.run(board.elapse(at + ms(10) - board.time))
    assert asyncio.run(reads(at)) == (waited, fast, later)


def test_read_until_takes_through_the_match(board: popcorn.Machine) -> None:
    uart = board.periph.USART1.port

    async def body() -> tuple[bytes, re.Match[bytes]] | None:
        await uart.write(b"abc\nrest")
        return await uart.read_until(re.compile(rb"\n"), for_=ms(20))

    found = asyncio.run(body())
    assert found is not None
    taken, match = found
    assert taken == b"abc\n"
    assert match.end() == 4
    asyncio.run(board.run(for_=ms(2)))
    assert uart.take() == b"rest"


async def chunks(m: popcorn.Machine, first: bytes, second: bytes) -> None:
    """Write `first` now and `second` 2 ms later."""
    uart = m.periph.USART1.port
    await uart.write(first)
    uart.write(second, at=m.time + ms(2))


@pytest.mark.parametrize(
    ("first", "second", "pattern", "taken"),
    [
        (b"rea", b"dy\n", rb"ready\n", b"ready\n"),
        (b"a", b"b", rb"(?<=a)b", b"ab"),
        (b"12", b"3x", rb"\d+", b"1"),
    ],
    ids=["spanning", "lookbehind", "greedy"],
)
def test_read_until_matches_across_chunks_at_the_first_boundary_holding_a_match(
    board: popcorn.Machine, first: bytes, second: bytes, pattern: bytes, taken: bytes
) -> None:
    uart = board.periph.USART1.port

    async def body() -> tuple[bytes, re.Match[bytes]] | None:
        await chunks(board, first, second)
        return await uart.read_until(re.compile(pattern), for_=ms(10))

    found = asyncio.run(body())
    assert found is not None
    assert found[0] == taken


@pytest.mark.parametrize("pattern", [rb"o", rb"o+"], ids=["fixed", "growing"])
def test_read_until_gives_the_same_bytes_and_span_parked_and_running(
    board: popcorn.Machine, pattern: bytes
) -> None:
    uart = board.periph.USART1.port
    start = board.snapshot()
    compiled = re.compile(pattern)

    async def parked() -> tuple[bytes, re.Match[bytes]] | None:
        await uart.write(b"xxoo")
        return await uart.read_until(compiled, for_=ms(10))

    async def running() -> tuple[bytes, re.Match[bytes]] | None:
        async with board.running():
            await uart.write(b"xxoo")
            return await uart.read_until(compiled, for_=ms(10))

    first = asyncio.run(parked())
    board.restore(start)
    second = run_bounded(running(), 10)
    assert first is not None and second is not None
    assert first[0] == second[0] == b"xxo"
    assert first[1].span() == second[1].span()


def test_a_growing_match_is_the_shortest_even_when_one_slice_saw_every_byte(
    u575: popcorn.MachineDescription,
) -> None:
    m = popcorn.Machine(
        u575,
        image=popcorn.Image.elf(BOARD_ELF),
        slice_instructions=1_000_000_000,
        slice_wall_budget=10.0,
    )
    m.reset()
    asyncio.run(m.run(until=m.sym("ready"), budget=1_000_000))
    uart = m.periph.USART1.port

    async def running() -> tuple[tuple[bytes, re.Match[bytes]] | None, bytes]:
        async with m.running():
            await uart.write(b"123x")
            found = await uart.read_until(re.compile(rb"\d+"), for_=ms(10))
            await m.elapse(ms(1))
            return found, uart.take()

    found, rest = run_bounded(running(), 30)
    assert found is not None
    assert found[0] == b"1"
    assert found[1].span() == (0, 1)
    assert rest == b"23x"


def test_read_until_runs_python_only_where_bytes_arrived(board: popcorn.Machine) -> None:
    uart = board.periph.USART1.port

    async def running() -> tuple[object, int]:
        async with board.running():
            await board.elapse(ms(1))
            before = board._handle.gil_entries()  # pyright: ignore[reportPrivateUsage]
            found = await uart.read_until(re.compile(rb"\n"), for_=ms(5))
            entries = board._handle.gil_entries() - before  # pyright: ignore[reportPrivateUsage]
            return found, entries

    found, entries = run_bounded(running(), 10)
    assert found is None
    assert entries == 0


@pytest.mark.parametrize(
    "pattern",
    [re.compile("ready"), b"ready", "ready"],
    ids=["str-regex", "bytes", "str"],
)
def test_read_until_refuses_a_pattern_that_is_no_bytes_regex_at_the_call(
    board: popcorn.Machine, pattern: object
) -> None:
    uart = board.periph.USART1.port

    async def running() -> VirtualTime:
        async with board.running():
            with pytest.raises(TypeError, match="compiled bytes regex"):
                await uart.read_until(pattern)  # pyright: ignore[reportArgumentType]
            return await board.elapse(ms(1))

    assert run_bounded(running(), 10) > VirtualTime(0)


def test_read_until_reports_bytes_the_full_queue_dropped(board: popcorn.Machine) -> None:
    uart = board.periph.USART1.port
    board._ports.link("USART1").set_frame_limit(2)  # pyright: ignore[reportPrivateUsage]

    async def body() -> tuple[bytes, re.Match[bytes]] | None:
        await uart.write(b"abcd")
        return await uart.read_until(re.compile(rb"z"), for_=ms(5))

    with pytest.raises(RuntimeError, match="dropped bytes"):
        asyncio.run(body())
    assert uart.available == 2


def test_read_until_refuses_a_pattern_matching_nothing(board: popcorn.Machine) -> None:
    with pytest.raises(ValueError, match="matches no bytes"):
        asyncio.run(board.periph.USART1.port.read_until(re.compile(rb"x*"), for_=ms(1)))


def test_read_until_takes_no_gil_parked_without_bytes(board: popcorn.Machine) -> None:
    uart = board.periph.USART1.port
    before = board._handle.gil_entries()  # pyright: ignore[reportPrivateUsage]
    assert asyncio.run(uart.read_until(re.compile(rb"\n"), for_=ms(5))) is None
    assert board._handle.gil_entries() == before  # pyright: ignore[reportPrivateUsage]


def reading_board(
    u575: popcorn.MachineDescription, cancel: bool
) -> tuple[weakref.ref[popcorn.Machine], weakref.ref[re.Pattern[bytes]]]:
    """A board machine whose free run ran a read_until, cancelled or matched, then ended."""
    m = popcorn.Machine(u575, image=popcorn.Image.elf(BOARD_ELF))
    m.reset()
    asyncio.run(m.run(until=m.sym("ready"), budget=1_000_000))
    uart = m.periph.USART1.port
    pattern = re.compile(b"x" if cancel else rb"\n")

    async def running() -> None:
        async with m.running():
            reading = asyncio.create_task(uart.read_until(pattern, for_=ms(50)))
            if cancel:
                await m.elapse(ms(1))
                reading.cancel()
                with contextlib.suppress(asyncio.CancelledError):
                    await reading
            else:
                await uart.write(b"ok\n")
                assert await reading is not None

    run_bounded(running(), 10)
    return weakref.ref(m), weakref.ref(pattern)


@pytest.mark.parametrize("cancel", [True, False], ids=["cancelled", "matched"])
def test_a_finished_read_until_keeps_neither_machine_nor_pattern(
    u575: popcorn.MachineDescription, cancel: bool
) -> None:
    machine, pattern = reading_board(u575, cancel)
    re.purge()
    gc.collect()
    assert machine() is None
    assert pattern() is None


def test_read_until_misses_without_taking(board: popcorn.Machine) -> None:
    uart = board.periph.USART1.port

    async def body() -> tuple[bytes, re.Match[bytes]] | None:
        await uart.write(b"no newline")
        return await uart.read_until(re.compile(rb"\n"), for_=ms(5))

    assert asyncio.run(body()) is None
    assert uart.available == 10
    assert uart.take() == b"no newline"


def test_read_until_stops_at_max_bytes_without_taking(board: popcorn.Machine) -> None:
    uart = board.periph.USART1.port

    async def body() -> tuple[bytes, re.Match[bytes]] | None:
        await uart.write(b"x" * 20)
        await board.run(for_=ms(5))
        return await uart.read_until(re.compile(rb"\n"), for_=ms(5), max_bytes=8)

    before = board.instructions
    assert asyncio.run(body()) is None
    assert uart.available == 20
    assert board.instructions > before
    with pytest.raises(ValueError, match="max_bytes"):
        asyncio.run(uart.read_until(re.compile(rb"x"), max_bytes=0))


def test_read_until_returns_a_queued_match_without_running(board: popcorn.Machine) -> None:
    uart = board.periph.USART1.port

    async def queued() -> None:
        await uart.write(b"ok\n")
        await board.run(for_=ms(2))

    asyncio.run(queued())
    before = board.instructions
    found = asyncio.run(uart.read_until(re.compile(rb"ok\n")))
    assert found is not None and found[0] == b"ok\n"
    assert board.instructions == before


def test_a_cancelled_read_until_takes_nothing(board: popcorn.Machine) -> None:
    uart = board.periph.USART1.port

    async def body() -> int:
        async with board.running():
            await uart.write(b"abc")
            reading = asyncio.create_task(uart.read_until(re.compile(rb"\n")))
            while uart.available < 3:
                await board.elapse(ms(1))
            reading.cancel()
            with contextlib.suppress(asyncio.CancelledError):
                await reading
            return uart.available

    assert run_bounded(body(), 10) == 3


def test_a_take_during_read_until_raises(board: popcorn.Machine) -> None:
    uart = board.periph.USART1.port

    async def body() -> None:
        async with board.running():
            await uart.write(b"abc")
            reading = asyncio.create_task(uart.read_until(re.compile(rb"\n"), for_=ms(50)))
            while uart.available < 3:
                await board.elapse(ms(1))
            await board.elapse(ms(1))
            uart.take()
            await uart.write(b"\n")
            await reading

    with pytest.raises(RuntimeError, match="took queued bytes"):
        run_bounded(body(), 10)


PERIPH_USART_ELF = Path(__file__).parents[2] / "firmware" / "periph" / "prebuilt" / "usart_ll.elf"


def break_sender(u575: popcorn.MachineDescription) -> popcorn.Machine:
    m = popcorn.Machine(u575, image=popcorn.Image.elf(PERIPH_USART_ELF))
    m.reset()
    stop = asyncio.run(m.run(until="main", budget=1_000_000))
    assert stop.reason is popcorn.StopReason.ADDRESS, stop
    m.reg.pc = m.sym("break_tx")
    return m


def test_a_break_reads_as_a_marked_frame_and_a_zero_byte(
    u575: popcorn.MachineDescription,
) -> None:
    marked = break_sender(u575)
    stop = asyncio.run(marked.run(budget=100_000_000))
    assert stop.reason is popcorn.StopReason.USER, stop
    frames = marked.periph.USART1.port.take_frames()
    assert [(f.data, f.brk) for f in frames] == [(ord("a"), False), (0, True), (ord("b"), False)]

    plain = break_sender(u575)
    assert asyncio.run(plain.periph.USART1.port.read(3, for_=ms(50))) == b"a\x00b"

    built = Frame(1, VirtualTime(5))
    assert built.brk is False
    assert Frame(0, VirtualTime(5), True).brk is True


def test_a_break_reaches_a_pty_as_a_zero_byte(u575: popcorn.MachineDescription) -> None:
    m = break_sender(u575)
    path = m.board.pty("USART1")
    fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    try:
        stop = asyncio.run(m.run(budget=100_000_000))
        assert stop.reason is popcorn.StopReason.USER, stop
        got = b""
        deadline = 50
        while len(got) < 3 and deadline > 0:
            ready, _, _ = select.select([fd], [], [], 0.1)
            if ready:
                got += os.read(fd, 16)
            deadline -= 1
        assert got == b"a\x00b"
    finally:
        os.close(fd)
