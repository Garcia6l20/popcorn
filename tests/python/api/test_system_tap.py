"""`System.tap`: frames on a link with their system times, injected, replaced and dropped
frames, and the streams of linked UARTs."""

from __future__ import annotations

import asyncio
import gc
import threading
import weakref
from collections.abc import Callable
from typing import cast

import popcorn
import pytest
from popcorn import Call, EmulationError, Return, System, TapFrame, UartTap, _core
from popcorn._refs import Pending
from popcorn.units import VirtualDuration, VirtualTime, ms, us

from .linked import (
    FIRMWARE_BAUD,
    LINK_BYTE,
    TX_BYTES,
    VARIANT_VALUES,
    VARIANTS,
    Build,
    L,
    Ticker,
    at,
    in_flight,
    links,
    logged,
    member,
    past_done,
    received,
    to,
)


def tap_pauses(system: System) -> list[list[TapFrame]]:
    """The frames the tap of `system`'s first link takes at each of seven TIME pauses."""
    tap = system.tap("a.USART1")
    seen: list[list[TapFrame]] = []
    for micros in (101, 257, 401, 733, 1001, 1777, 2999):
        asyncio.run(to(system, at(micros)))
        frames = tap.frames()
        assert all(frame.end <= at(micros) for frame in frames)
        seen.append(frames)
    return seen


def test_the_tap_at_each_pause_holds_the_frames_ended_by_it_at_any_quantum(
    u575: popcorn.PartDescription, pair: Build
) -> None:
    results: list[tuple[list[list[TapFrame]], list[int]]] = []
    for slice_instructions, third in VARIANT_VALUES:
        system = pair(slice_instructions, third)
        results.append((tap_pauses(system), logged(system.members["b"])))
    assert all(result == results[0] for result in results[1:])
    seen, _ = results[0]
    every = [frame for frames in seen for frame in frames]
    assert [frame.data for frame in every] == TX_BYTES
    assert [frame.index for frame in every] == list(range(64))
    assert all(frame.towards == "b.USART1" and frame.arrival == frame.end + L for frame in every)
    assert not any(frame.injected or frame.brk or frame.lost or frame.dropped for frame in every)
    assert sum(1 for frames in seen if frames) >= 4
    lone = member(u575, "link_tx", "a")
    boot = lone.time
    asyncio.run(lone.run(for_=ms(3)))
    assert [(f.data, f.end.ps) for f in every] == [
        (f.data, f.at.ps - boot.ps) for f in lone.periph.USART1.port.take_frames()
    ]
    assert [word & 0xFFFF for word in results[0][1][::2] if word >> 16 == LINK_BYTE] == TX_BYTES


def injected_run(system: System, running: bool) -> tuple[tuple[int, ...], tuple[TapFrame, ...]]:
    """Inject b"xyz" towards b at 4 ms, from a stopped system at 3 ms or from the system's free
    run asleep on IDLE, then run to 6 ms: b's log and the tap from 3 ms on."""
    asyncio.run(to(system, at(3000)))
    past_done(system)
    tap = system.tap("b.USART1")
    tap.frames()
    b = system.members["b"]

    async def free() -> None:
        async with system.running():
            await b.elapse(us(100))
            await in_flight(system)
            tap.inject(b"xyz", at=at(4000), towards="b.USART1")
            await b.elapse(ms(1.5))

    if running:
        asyncio.run(free())
    else:
        tap.inject(b"xyz", at=at(4000), towards="b.USART1")
    asyncio.run(to(system, at(6000)))
    return tuple(logged(b)), tuple(tap.frames())


def lone_written(u575: popcorn.PartDescription) -> tuple[int, ...]:
    """The last log words of a lone link_rx machine whose port writes b"xyz" at 4 ms of system
    time: three bytes and an IDLE, each with its cycle count."""
    lone = member(u575, "link_rx", "b")
    boot = lone.time

    async def go() -> None:
        await lone.run(for_=ms(3))
        pending = lone.periph.USART1.port.write(b"xyz", at=VirtualTime(boot.ps + ms(4).ps))
        await lone.run(for_=ms(3))
        await pending

    asyncio.run(go())
    return tuple(logged(lone)[-8:])


@pytest.mark.parametrize(("slice_instructions", "third"), VARIANTS)
def test_frames_injected_running_equal_those_injected_stopped_and_a_lone_write(
    u575: popcorn.PartDescription,
    pair: Build,
    slice_instructions: int,
    third: VirtualDuration | None,
) -> None:
    reference = injected_run(pair(baud=FIRMWARE_BAUD), False)
    assert injected_run(pair(slice_instructions, third, baud=FIRMWARE_BAUD), False) == reference
    assert injected_run(pair(slice_instructions, third, baud=FIRMWARE_BAUD), True) == reference
    log, shown = reference
    assert log[-8:] == lone_written(u575)
    words = [word for word in log[::2] if word >> 16 == LINK_BYTE]
    assert [word & 0xFFFF for word in words] == [*TX_BYTES, *b"xyz"]
    assert [(f.data, f.index, f.injected, f.towards) for f in shown] == [
        (byte, index, True, "b.USART1") for index, byte in enumerate(b"xyz")
    ]
    assert shown[0].start == at(4000)
    assert all(f.arrival == f.end for f in shown)
    assert [f.start for f in shown[1:]] == [f.end for f in shown[:-1]]


def test_an_injection_stopped_is_checked_at_the_call(pair: Build) -> None:
    system = pair()
    asyncio.run(to(system, at(3000)))
    tap = system.tap("a.USART1")
    now = system.time_of("b")
    with pytest.raises(ValueError, match=r"before b\.USART1's system time"):
        tap.inject(b"x", at=VirtualTime(now.ps - 1), towards="b.USART1")
    tap.inject(b"xy", at=at(4000), towards="b.USART1")
    with pytest.raises(ValueError, match=r"overlaps the frame starting at t=4000000000 ps"):
        tap.inject(b"z", at=at(4010), towards="b.USART1")
    tap.inject(b"z", at=at(4010), towards="a.USART1")


async def overlapping(system: System, tap: UartTap) -> None:
    """Inject, from the free run, a frame at 4010 us overlapping one injected at 4000 us."""
    b = system.members["b"]
    async with system.running():
        await b.elapse(us(100))
        await in_flight(system)
        tap.inject(b"xy", at=at(4000), towards="b.USART1")
        tap.inject(b"z", at=at(4010), towards="b.USART1")
        await b.elapse(ms(1.5))


def test_an_overlap_injected_while_running_stops_the_system_at_its_start(pair: Build) -> None:
    errors: list[str] = []
    for slice_instructions, third in VARIANT_VALUES:
        system = pair(slice_instructions, third)
        asyncio.run(to(system, at(3000)))
        past_done(system)
        tap = system.tap("b.USART1")
        with pytest.raises(EmulationError, match="the run this wait was in ended") as raised:
            asyncio.run(overlapping(system, tap))
        notes = getattr(raised.value, "__notes__", [])
        assert len(notes) == 1 and "overlap" in notes[0]
        errors.append(notes[0])
        with pytest.raises(EmulationError, match="stopped on an error"):
            tap.inject(b"x", at=at(9000), towards="b.USART1")
    assert all(error == errors[0] for error in errors)
    assert "4010000000 ps" in errors[0]


def edited_run(system: System) -> tuple[object, ...]:
    tap = system.tap("a.USART1")
    assert tap.posted(towards="b.USART1") == 0
    tap.replace(40, b"abc", towards="b.USART1")
    tap.drop(10, 2, towards="b.USART1")
    with pytest.raises(ValueError, match="overlap an edit armed before"):
        tap.drop(42, towards="b.USART1")
    asyncio.run(to(system, at(3000)))
    assert tap.posted(towards="b.USART1") == 64
    with pytest.raises(ValueError, match="started already: 64 frames started"):
        tap.replace(63, b"q", towards="b.USART1")
    return tuple(logged(system.members["b"])), tuple(tap.frames())


@pytest.mark.parametrize(("slice_instructions", "third"), VARIANTS)
def test_replaced_and_dropped_frames_reach_the_receiver_at_their_own_times(
    pair: Build, slice_instructions: int, third: VirtualDuration | None
) -> None:
    log, frames = edited_run(pair(slice_instructions, third))
    plain = pair()
    asyncio.run(to(plain, at(3000)))
    plain_frames = plain.tap("a.USART1").frames()
    shown = cast(tuple[TapFrame, ...], frames)
    expected = list(TX_BYTES)
    expected[40:43] = list(b"abc")
    assert [f.data for f in shown] == expected
    assert [f.index for f in shown if f.dropped] == [10, 11]
    assert [(f.start, f.end) for f in shown] == [(f.start, f.end) for f in plain_frames]
    words = [word for word in cast(tuple[int, ...], log)[::2] if word >> 16 == LINK_BYTE]
    assert [word & 0xFFFF for word in words] == expected[:10] + expected[12:]
    assert edited_run(pair(slice_instructions, third)) == (log, frames)


def test_a_linked_uart_s_stream_refuses_writes_and_reads_its_own_transmissions(
    pair: Build,
) -> None:
    system = pair()
    a = system.members["a"]
    with pytest.raises(RuntimeError, match="inject through the system's tap"):
        a.periph.USART1.port.write(b"x")
    with pytest.raises(RuntimeError, match="inject through the system's tap"):
        a.periph.USART1.port.write_break()
    with pytest.raises(RuntimeError, match="the link carries no flow control"):
        a.periph.USART1.port.set_cts(False)
    assert asyncio.run(a.periph.USART1.port.read(4)) == bytes(TX_BYTES[:4])
    a.halt()
    system.close()

    async def write() -> VirtualTime:
        return await a.periph.USART1.port.write(b"x")

    assert asyncio.run(write()) == a.time


def test_tap_arguments_are_checked_at_the_call(pair: Build) -> None:
    system = pair()
    tap = system.tap("a.USART1")
    for end in ("a.USART3", "c.USART1", "a", ""):
        with pytest.raises(ValueError, match="no end of a link"):
            system.tap(end)
    with pytest.raises(ValueError, match=r"neither a\.USART1 nor b\.USART1"):
        tap.posted(towards="a.USART2")
    with pytest.raises(TypeError, match="towards must be a str"):
        tap.posted(towards=cast(str, 1))
    with pytest.raises(TypeError, match="data must be bytes"):
        tap.inject(cast(bytes, "x"), at=at(10), towards="b.USART1")
    with pytest.raises(ValueError, match="at least one byte"):
        tap.inject(b"", at=at(10), towards="b.USART1")
    with pytest.raises(TypeError, match="at must be a VirtualTime"):
        tap.inject(b"x", at=cast(VirtualTime, 10), towards="b.USART1")
    with pytest.raises(ValueError, match="index must be at least 0"):
        tap.replace(-1, b"x", towards="b.USART1")
    with pytest.raises(TypeError, match="index must be an int"):
        tap.drop(cast(int, True), towards="b.USART1")
    with pytest.raises(ValueError, match="n must be at least 1"):
        tap.drop(3, 0, towards="b.USART1")
    assert repr(tap) == "UartTap(a.USART1 <-> b.USART1)"


def test_a_closed_system_s_tap_holds_every_frame_ended_by_its_last_pause(
    u575: popcorn.PartDescription,
) -> None:
    a = member(u575, "link_tx", "a")
    system = System({"a": a, "b": member(u575, "link_rx", "b")}, links(None))
    tap = system.tap("a.USART1")
    asyncio.run(to(system, at(1001)))
    system.close()
    frames = tap.frames()
    assert frames and all(f.end <= at(1001) for f in frames)
    assert [f.data for f in frames] == TX_BYTES[: len(frames)]
    with pytest.raises(EmulationError, match="closed"):
        tap.inject(b"x", at=at(2000), towards="b.USART1")
    with pytest.raises(EmulationError, match="closed"):
        tap.drop(63, towards="b.USART1")
    held = weakref.ref(system)
    del system, tap, frames
    gc.collect()
    assert held() is None
    assert a.time.ps >= us(1001).ps


def test_the_tap_keeps_its_limit_and_counts_what_it_dropped(
    pair: Build, monkeypatch: pytest.MonkeyPatch
) -> None:
    from popcorn import _system

    monkeypatch.setattr(_system, "_TAP_KEEP", 8)
    system = pair()
    tap = system.tap("a.USART1")
    asyncio.run(to(system, at(3000)))
    assert (tap.available, tap.dropped) == (8, 56)
    assert [f.index for f in tap.frames()] == list(range(56, 64))
    assert (tap.available, tap.dropped) == (0, 56)


def test_an_injection_that_finds_the_system_running_takes_the_running_path(pair: Build) -> None:
    system = pair(baud=FIRMWARE_BAUD)
    asyncio.run(to(system, at(3000)))
    past_done(system)
    tap = system.tap("b.USART1")
    b = system.members["b"]

    async def free() -> None:
        async with system.running():
            await b.elapse(us(100))
            await in_flight(system)
            flight = system._flight  # pyright: ignore[reportPrivateUsage]
            system._flight = None  # pyright: ignore[reportPrivateUsage]
            try:
                tap.inject(b"xyz", at=at(4000), towards="b.USART1")
            finally:
                system._flight = flight  # pyright: ignore[reportPrivateUsage]
            await b.elapse(ms(1.5))

    asyncio.run(free())
    asyncio.run(to(system, at(6000)))
    assert tuple(logged(b)[-8:]) == injected_run(pair(baud=FIRMWARE_BAUD), False)[0][-8:]


def test_frames_take_one_direction_or_both_in_end_order(pair: Build) -> None:
    system = pair(a_entry="link_ping", b_entry="link_echo")
    tap = system.tap("b.USART1")
    asyncio.run(to(system, at(3000)))
    assert tap.available == 32
    towards_a = tap.frames(towards="a.USART1")
    assert [f.data for f in towards_a] == [0x40 + index for index in range(16)]
    assert {f.towards for f in towards_a} == {"a.USART1"}
    assert tap.available == 16
    assert tap.frames(towards="a.USART1") == []
    asyncio.run(to(system, at(3001)))
    plain = pair(a_entry="link_ping", b_entry="link_echo")
    asyncio.run(to(plain, at(3000)))
    both = plain.tap("a.USART1").frames()
    assert [f.towards for f in both[:2]] == ["b.USART1", "a.USART1"]
    assert [f.end for f in both] == sorted(f.end for f in both)
    assert [f for f in both if f.towards == "b.USART1"] == tap.frames()


def test_a_linked_uart_refuses_a_pty_until_its_system_closes(pair: Build) -> None:
    system = pair()
    a = system.members["a"]
    with pytest.raises(RuntimeError, match="USART1 is linked in a System"):
        a.board.pty("USART1")
    assert a.board.ptys == {}
    system.close()
    assert a.board.pty("USART1").startswith("/dev/")


def test_a_system_collected_unclosed_gives_its_uarts_back(
    u575: popcorn.PartDescription,
) -> None:
    a = member(u575, "link_tx", "a")
    system = System({"a": a, "b": member(u575, "link_rx", "b")}, links(None))
    asyncio.run(to(system, at(100)))
    with pytest.raises(RuntimeError, match="linked in a System"):
        a.periph.USART1.port.write(b"x")
    held = weakref.ref(system)
    del system
    gc.collect()
    assert held() is None

    async def write() -> VirtualTime:
        return await a.periph.USART1.port.write(b"x")

    assert asyncio.run(write()) == a.time
    assert a.board.pty("USART1").startswith("/dev/")


class HeldTake:
    """A system handle whose `tap_take`, on the thread named "held", sets `took` and waits for
    `go` after taking frames: another reader then takes and appends later frames while that
    one still holds its own."""

    def __init__(self, handle: _core.SystemHandle) -> None:
        self._handle = handle
        self.took = threading.Event()
        self.go = threading.Event()

    def tap_take(self, wire: int) -> list[_core.TapRecord]:
        taken = self._handle.tap_take(wire)
        if taken and threading.current_thread().name == "held":
            self.took.set()
            self.go.wait(10)
        return taken

    def __getattr__(self, name: str) -> object:
        return getattr(self._handle, name)


def test_frames_taken_by_two_threads_at_once_keep_each_direction_in_order(pair: Build) -> None:
    system = pair()
    tap = system.tap("a.USART1")
    held = HeldTake(system._handle)  # pyright: ignore[reportPrivateUsage]
    system._handle = cast(_core.SystemHandle, held)  # pyright: ignore[reportPrivateUsage]
    asyncio.run(to(system, at(1001)))
    holding = threading.Thread(target=lambda: tap.available, name="held")
    holding.start()
    assert held.took.wait(10)
    asyncio.run(to(system, at(3000)))
    threading.Timer(0.2, held.go.set).start()
    first = tap.frames(towards="b.USART1")
    holding.join(10)
    every = first + tap.frames(towards="b.USART1")
    assert [f.index for f in every] == list(range(64))


TAP_READS: list[Callable[[], None]] = []


class TapReader(Ticker):
    """A Ticker every microsecond that calls each function in `TAP_READS` at each tick."""

    period_us = 1

    def _tick(self) -> None:
        super()._tick()
        for read in TAP_READS:
            read()


def read_from_receiver(
    u575: popcorn.PartDescription, slice_instructions: int, third: VirtualDuration | None
) -> tuple[list[tuple[VirtualTime, int]], list[TapFrame], VirtualTime]:
    """Read the tap every microsecond of b's time from b's TIM6 model while a runs free and b
    runs for 1003 us, then at b's stop: each read's time with the frames held by then, every
    frame read, and b's stop."""
    a = member(u575, "link_tx", "a", slice_instructions)
    b = member(u575, "link_rx", "b", slice_instructions, models={"TIM6": TapReader})
    shown: list[TapFrame] = []
    reads: list[tuple[VirtualTime, int]] = []
    with System({"a": a, "b": b}, links(third), slice_instructions=slice_instructions) as system:
        tap = system.tap("a.USART1")

        def read() -> None:
            shown.extend(tap.frames(towards="b.USART1"))
            reads.append((system.to_system("b", b.time), len(shown)))

        async def body() -> None:
            async with a.running():
                await b.run(for_=us(1003))

        TAP_READS.append(read)
        try:
            asyncio.run(body())
        finally:
            TAP_READS.clear()
        shown.extend(tap.frames(towards="b.USART1"))
        return reads, shown, system.time_of("b")


@pytest.mark.parametrize(("slice_instructions", "third"), VARIANTS)
def test_a_tap_read_from_a_member_callback_lags_two_windows_and_the_pause_completes_it(
    u575: popcorn.PartDescription,
    pair: Build,
    slice_instructions: int,
    third: VirtualDuration | None,
) -> None:
    whole = pair()
    asyncio.run(to(whole, at(3000)))
    every = whole.tap("a.USART1").frames(towards="b.USART1")
    reads, shown, point = read_from_receiver(u575, slice_instructions, third)
    assert len(reads) > 900
    assert shown == [frame for frame in every if frame.end <= point]
    quantum = L if third is None else min(L, third, key=lambda one: one.ps)
    for when, held in reads:
        due = sum(1 for frame in every if frame.end.ps <= when.ps - 2 * quantum.ps)
        assert held >= due, (when, held, due)


def clash_before_a_peer_frame(system: System, start: VirtualTime) -> tuple[str, list[TapFrame]]:
    """Inject one frame towards b at `start` from the free run, while b's USART1 interrupt
    holds b 25 to 60 us before it: the error note and the tap at the error pause."""
    b = system.members["b"]
    tap = system.tap("a.USART1")
    reached = threading.Event()
    go = threading.Event()

    def on_irq(call: Call) -> Return:
        now = system.to_system("b", b.time).ps
        if not reached.is_set() and start.ps - us(60).ps <= now <= start.ps - us(25).ps:
            reached.set()
            assert go.wait(10)
        return call.proceed()

    b.intercept("USART1_IRQHandler", on_irq)

    async def body() -> None:
        async with system.running():
            assert await asyncio.to_thread(reached.wait, 10)
            try:
                tap.inject(b"x", at=start, towards="b.USART1")
            finally:
                go.set()
            await b.elapse(ms(1))

    with pytest.raises(EmulationError) as raised:
        asyncio.run(body())
    notes = [str(raised.value), *getattr(raised.value, "__notes__", [])]
    clash = [note for note in notes if "overlap" in note]
    assert len(clash) == 1, notes
    return clash[0], tap.frames()


@pytest.mark.parametrize(("slice_instructions", "third"), VARIANTS)
def test_a_running_injection_meeting_an_earlier_drained_peer_frame_errs_at_its_start(
    pair: Build, slice_instructions: int, third: VirtualDuration | None
) -> None:
    whole = pair()
    asyncio.run(to(whole, at(1000)))
    every = whole.tap("a.USART1").frames(towards="b.USART1")
    for after_send in (us(3), L - us(5)):
        start = VirtualTime(every[5].start.ps + after_send.ps)
        assert every[4].end + L > start
        note, frames = clash_before_a_peer_frame(pair(slice_instructions, third), start)
        assert f"injected frame 0 starts at {start.ps} ps" in note
        assert frames == [frame for frame in every if frame.end <= start]


async def polled(tap: UartTap, towards: str) -> TapFrame:
    """The first frame the tap shows towards `towards`, polled while the system runs."""
    while not (frames := tap.frames(towards=towards)):
        await asyncio.sleep(0.001)
    return frames[0]


@pytest.mark.timeout(30)
@pytest.mark.parametrize(("slice_instructions", "third"), [VARIANTS[0], VARIANTS[2]])
def test_a_host_polling_the_tap_of_a_sleeping_free_run_sees_each_echo_and_injects_the_next(
    pair: Build, slice_instructions: int, third: VirtualDuration | None
) -> None:
    system = pair(slice_instructions, third, a_entry="link_rx", b_entry="link_echo")
    asyncio.run(to(system, at(600)))
    tap = system.tap("a.USART1")

    async def body() -> list[TapFrame]:
        echoes: list[TapFrame] = []
        start = at(700)
        async with system.running():
            for data in (b"q", b"r"):
                tap.inject(data, at=start, towards="b.USART1")
                echoes.append(await polled(tap, "a.USART1"))
                start = echoes[-1].end + us(200)
        return echoes

    echoes = asyncio.run(body())
    assert [frame.data for frame in echoes] == [ord("q"), ord("r")]
    assert received(system.members["a"]) == [ord("q"), ord("r")]


def test_a_uart_whose_cts_is_held_or_pending_is_refused_a_link(
    u575: popcorn.PartDescription,
) -> None:
    a = member(u575, "link_tx", "a")
    b = member(u575, "link_rx", "b")
    port = a.periph.USART1.port
    asyncio.run(settle(port.set_cts(False)))
    asyncio.run(a.run(for_=us(10)))
    with pytest.raises(popcorn.ConfigError, match=r"links\[0\]\.a: a\.USART1 has its CTS"):
        System({"a": a, "b": b}, links(None))
    asyncio.run(settle(port.set_cts(True)))
    asyncio.run(a.run(for_=us(10)))
    asyncio.run(settle(port.set_cts(True, at=a.time + ms(1))))
    with pytest.raises(popcorn.ConfigError, match="a set_cts change pending"):
        System({"a": a, "b": b}, links(None))
    asyncio.run(a.run(for_=ms(2)))
    system = System({"a": a, "b": b}, links(None))
    asyncio.run(to(system, at(100)))
    system.close()


async def settle(pending: Pending[VirtualTime]) -> VirtualTime:
    return await pending
