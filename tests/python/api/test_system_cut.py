"""`UartTap.cut`, `break_line`, `restore_line` and `owner`: a link's line cut over a window of
system time, scheduled, from the cut owner's callbacks, stopped, and the refusals."""

from __future__ import annotations

import asyncio
import gc
import weakref
from collections.abc import Callable
from typing import cast

import popcorn
import pytest
from popcorn import (
    Call,
    MachineRunning,
    ResetKind,
    Return,
    StopReason,
    System,
    TapFrame,
    UartLink,
)
from popcorn.units import VirtualDuration, VirtualTime, ms, us

from .linked import (
    FIRMWARE_BAUD,
    LINK_BYTE,
    TX_BYTES,
    VARIANT_VALUES,
    VARIANTS,
    Build,
    L,
    at,
    in_flight,
    links,
    logged,
    member,
    past_done,
    to,
)

LINK_IDLE = 2
LINK_ERROR = 4
FE_ENTRY = LINK_ERROR << 16 | 1
CYCLES_PER_FRAME = 3470
STOP_SAMPLE = 153 / 160
FRAME_PS = 10 * 10**12 / FIRMWARE_BAUD
WINDOW = (at(600), at(1050))

Outcome = tuple[tuple[TapFrame, ...], tuple[int, ...]]
Seen = list[tuple[VirtualTime, VirtualTime]]


def finish(system: System, until: float = 3000) -> Outcome:
    """Run to `until` us and return the whole tap and b's log."""
    asyncio.run(to(system, at(until)))
    return tuple(system.tap("a.USART1").frames()), tuple(logged(system.members["b"]))


def scheduled(
    pair: Build,
    variant: tuple[int, VirtualDuration | None],
    start: VirtualTime,
    end: VirtualTime,
    line: popcorn.LineState = "idle",
) -> Outcome:
    system = pair(*variant)
    system.tap("a.USART1").cut(start, end, line=line)
    return finish(system)


def bytes_of(log: tuple[int, ...]) -> list[int]:
    return [word & 0xFFFF for word in log[::2] if word >> 16 == LINK_BYTE]


def kinds_between(log: tuple[int, ...], first: int, last: int) -> list[int]:
    """The log words between the entries of bytes `first` and `last`, cycle counts dropped."""
    words = list(log[::2])
    begin = words.index(LINK_BYTE << 16 | first)
    return words[begin + 1 : words.index(LINK_BYTE << 16 | last, begin)]


def outcome(call: Callable[[], object]) -> str:
    """What `call` did: "ok", or the exception's type and message."""
    try:
        call()
    except Exception as error:
        return f"{type(error).__name__}: {error}"
    return "ok"


@pytest.mark.parametrize(("slice_instructions", "third"), VARIANTS)
def test_a_scheduled_idle_cut_classifies_frames_by_their_sender_times(
    pair: Build, slice_instructions: int, third: VirtualDuration | None
) -> None:
    frames, log = scheduled(pair, (slice_instructions, third), *WINDOW)
    assert (frames, log) == scheduled(pair, (1_000_000, None), *WINDOW)
    assert [(f.index, f.cut, f.lost) for f in frames] == [
        (index, 12 <= index <= 31, index in (12, 31)) for index in range(64)
    ]
    assert not any(f.dropped or f.brk for f in frames)
    plain, _ = scheduled(pair, (1_000_000, None), at(5000), at(6000))
    assert [(f.start, f.end) for f in frames] == [(f.start, f.end) for f in plain]
    assert bytes_of(log) == TX_BYTES[:12] + TX_BYTES[32:]
    assert kinds_between(log, TX_BYTES[11], TX_BYTES[32]) == [LINK_IDLE << 16]
    words = list(log)
    byte = words.index(LINK_BYTE << 16 | TX_BYTES[11])
    idle_after = (WINDOW[0].ps - frames[11].end.ps) / FRAME_PS + 1
    assert abs(words[byte + 3] - words[byte + 1] - CYCLES_PER_FRAME * idle_after) <= 20


@pytest.mark.parametrize(("slice_instructions", "third"), VARIANTS)
def test_a_scheduled_break_cut_gives_one_break_character_at_its_stop_sample(
    pair: Build, slice_instructions: int, third: VirtualDuration | None
) -> None:
    frames, log = scheduled(pair, (slice_instructions, third), *WINDOW, line="break")
    assert (frames, log) == scheduled(pair, (1_000_000, None), *WINDOW, line="break")
    idle, _ = scheduled(pair, (1_000_000, None), *WINDOW)
    assert frames == idle
    assert kinds_between(log, TX_BYTES[11], TX_BYTES[32]) == [FE_ENTRY, LINK_BYTE << 16]
    words = list(log)
    byte = words.index(LINK_BYTE << 16 | TX_BYTES[11])
    error = words.index(FE_ENTRY)
    assert abs(words[error + 1] - words[byte + 1] - CYCLES_PER_FRAME * STOP_SAMPLE) <= 20


def owner_cut(system: System) -> tuple[Seen, Outcome]:
    """Inside `system.running()`, cut from a's first wait_complete call to its second, with
    `break_line` and `restore_line` in an intercept on a: the times returned and a's system
    time at each call, then the run to 3 ms."""
    a, b = system.members["a"], system.members["b"]
    tap = system.tap("a.USART1")
    seen: Seen = []

    def at_complete(call: Call) -> Return:
        own = system.to_system("a", a.time)
        if call.occurrence == 0:
            seen.append((tap.break_line(), own))
        elif call.occurrence == 1:
            seen.append((tap.restore_line(), own))
        return call.proceed()

    a.intercept("wait_complete", at_complete)

    async def free() -> None:
        async with system.running():
            await b.elapse(us(1500))

    asyncio.run(free())
    return seen, finish(system)


def receiver_outcome(system: System, until: float = 3000) -> Outcome:
    """Run to `until` us and return the whole tap and a's log, a receiving."""
    asyncio.run(to(system, at(until)))
    return tuple(system.tap("a.USART1").frames()), tuple(logged(system.members["a"]))


def owner_paused_cut(system: System) -> tuple[Seen, Outcome]:
    """a receives b's frames over a 50 us link and owns it. Inside `system.running()`, a's
    wait to 525 us makes a goal-stop pause with b ahead to 550 us, its frame 10 started at
    542.4 us. a's interrupts break the line at the first one at or after 525 us, frame 6's at
    527.3 us, and restore it at the next, the IDLE the cut gives."""
    a = system.members["a"]
    tap = system.tap("a.USART1")
    seen: Seen = []

    def on_irq(call: Call) -> Return:
        own = system.to_system("a", a.time)
        if not seen and own >= at(525):
            seen.append((tap.break_line(), own))
        elif len(seen) == 1:
            seen.append((tap.restore_line(), own))
        return call.proceed()

    a.intercept("USART1_IRQHandler", on_irq)

    async def free() -> None:
        async with system.running():
            await a.elapse(us(525))
            await a.elapse(us(900))

    asyncio.run(free())
    return seen, receiver_outcome(system)


def test_break_and_restore_from_the_owner_s_intercepts_take_its_system_time(
    pair: Build,
) -> None:
    results = [owner_cut(pair(*variant)) for variant in VARIANT_VALUES]
    seen, done = results[0]
    assert all(result == results[0] for result in results[1:])
    assert len(seen) == 2
    assert all(returned == own for returned, own in seen)
    (start, _), (end, _) = seen
    assert at(700) < start < end < at(1100)
    assert done == scheduled(pair, (1_000_000, None), start, end)
    frames, _ = done
    assert any(f.cut for f in frames)


def wide_pair(
    u575: popcorn.PartDescription, made: list[System], variant: tuple[int, VirtualDuration | None]
) -> System:
    """a receiving on link_rx from b on link_tx over a 50 us link, the quantum 50 us or the
    variant's third link latency."""
    slice_instructions, third = variant
    a = member(u575, "link_rx", "a", slice_instructions)
    b = member(u575, "link_tx", "b", slice_instructions)
    wide = [UartLink("a.USART1", "b.USART1", latency=us(50), baud=460_800)]
    if third is not None:
        wide.append(UartLink("a.USART2", "b.USART2", latency=third, baud=460_800))
    system = System({"a": a, "b": b}, wide, slice_instructions=slice_instructions)
    made.append(system)
    return system


def test_a_cut_from_the_owner_s_callback_after_a_pause_misses_no_frame_the_peer_sent_ahead(
    u575: popcorn.PartDescription,
) -> None:
    made: list[System] = []
    try:
        paused_owner_legs(u575, made)
    finally:
        for system in made:
            system.close()


def paused_owner_legs(u575: popcorn.PartDescription, made: list[System]) -> None:
    results = [owner_paused_cut(wide_pair(u575, made, variant)) for variant in VARIANT_VALUES]
    seen, done = results[0]
    assert all(result == results[0] for result in results[1:])
    assert len(seen) == 2
    assert all(returned == own for returned, own in seen)
    (start, _), (end, _) = seen
    assert at(525) <= start < at(530)
    reference = wide_pair(u575, made, (1_000_000, None))
    reference.tap("a.USART1").cut(start, end)
    assert done == receiver_outcome(reference)
    frames, _ = done
    assert [f.index for f in frames if f.cut][:2] == [9, 10]


def test_break_line_from_the_body_while_running_raises_and_changes_nothing(
    pair: Build,
) -> None:
    system = pair()
    a, b = system.members["a"], system.members["b"]
    tap = system.tap("a.USART1")
    seen: Seen = []

    def at_complete(call: Call) -> Return:
        own = system.to_system("a", a.time)
        if call.occurrence == 0:
            seen.append((tap.break_line(), own))
        elif call.occurrence == 1:
            seen.append((tap.restore_line(), own))
        return call.proceed()

    a.intercept("wait_complete", at_complete)

    async def free() -> None:
        async with system.running():
            await b.elapse(us(100))
            await in_flight(system)
            with pytest.raises(RuntimeError, match="from a callback of a, the link's cut owner"):
                tap.break_line()
            with pytest.raises(RuntimeError, match=r"or call cut\(start, end\) from here"):
                tap.restore_line()
            await b.elapse(us(1400))

    asyncio.run(free())
    done = finish(system)
    (start, _), (end, _) = seen
    assert done == scheduled(pair, (1_000_000, None), start, end)


def two_callers(system: System) -> tuple[list[str], list[str]]:
    """Intercepts on a and b both break then restore the line in the same stretch of time."""
    a, b = system.members["a"], system.members["b"]
    tap = system.tap("a.USART1")
    by_a: list[str] = []
    by_b: list[str] = []

    def on_a(call: Call) -> Return:
        if call.occurrence in (0, 1):
            by_a.append(outcome(tap.break_line if call.occurrence == 0 else tap.restore_line))
        return call.proceed()

    def on_b(call: Call) -> Return:
        if call.occurrence in (38, 39):
            by_b.append(outcome(tap.break_line if call.occurrence == 38 else tap.restore_line))
        return call.proceed()

    a.intercept("wait_complete", on_a)
    b.intercept("USART1_IRQHandler", on_b)
    finish(system)
    return by_a, by_b


def test_only_the_cut_owner_s_callbacks_change_cuts_whichever_calls_first(pair: Build) -> None:
    refused_a = "RuntimeError: break_line from a callback: only callbacks of a, the link's"
    for slice_instructions, third in VARIANT_VALUES:
        by_a, by_b = two_callers(pair(slice_instructions, third))
        assert by_a == ["ok", "ok"]
        assert len(by_b) == 2 and by_b[0].startswith(refused_a)
        assert by_b[1].startswith("RuntimeError: restore_line from a callback: only callbacks of a")
        swapped = pair(slice_instructions, third)
        swapped.tap("a.USART1").owner = "b"
        by_a, by_b = two_callers(swapped)
        assert by_b == ["ok", "ok"]
        assert [line.split(":")[0] for line in by_a] == ["RuntimeError", "RuntimeError"]
        assert "only callbacks of b" in by_a[0]


def test_a_callback_of_another_system_s_machine_is_refused(pair: Build) -> None:
    system = pair()
    tap = system.tap("a.USART1")
    other = pair()
    tried: list[str] = []

    def on_other(call: Call) -> Return:
        if call.occurrence == 0:
            tried.append(outcome(tap.break_line))
            tried.append(outcome(lambda: tap.cut(at(5000), at(6000))))
        return call.proceed()

    other.members["a"].intercept("wait_complete", on_other)
    finish(other)
    assert [line.split(":")[0] for line in tried] == ["RuntimeError", "RuntimeError"]
    assert all("from a callback of a machine outside this system" in line for line in tried)
    assert all(
        "only callbacks of a, the link's cut owner, change its cuts" in line for line in tried
    )
    with pytest.raises(ValueError, match="no cut is open"):
        tap.restore_line()
    assert tap.break_line() == at(0)


def body_after_a_pause(system: System) -> None:
    """Inside running(), the body calls break_line and sets the owner right after a wait that
    a pause resolved: both raise, nothing changes."""
    a = system.members["a"]
    tap = system.tap("a.USART1")

    async def free() -> None:
        async with system.running():
            await a.elapse(us(300))
            with pytest.raises(RuntimeError, match="cut owner"):
                tap.break_line()
            with pytest.raises(MachineRunning):
                tap.owner = "b"

    asyncio.run(free())
    with pytest.raises(ValueError, match="no cut is open"):
        tap.restore_line()
    assert tap.owner == "a"


def test_a_body_call_after_a_pause_inside_running_or_with_a_goal_pending_raises(
    pair: Build,
) -> None:
    for _ in range(20):
        body_after_a_pause(pair())
    system = pair()
    a = system.members["a"]
    tap = system.tap("a.USART1")

    async def goal() -> None:
        pending = asyncio.ensure_future(a.run(for_=us(300)))
        await asyncio.sleep(0)
        with pytest.raises(RuntimeError, match="cut owner"):
            tap.break_line()
        with pytest.raises(MachineRunning, match="a goal of a is pending"):
            tap.owner = "b"
        await pending

    asyncio.run(goal())
    with pytest.raises(ValueError, match="no cut is open"):
        tap.restore_line()
    assert tap.owner == "a"


def test_the_caller_is_found_by_its_engine_and_a_parked_section_is_refused(
    u575: popcorn.PartDescription,
) -> None:
    a = member(u575, "link_tx", "a")
    b = member(u575, "link_rx", "b")
    c = member(u575, "link_tx", "c")
    system = System(
        {"a": a, "b": b, "c": c},
        [
            UartLink("a.USART1", "b.USART1", latency=L, baud=460_800),
            UartLink("b.USART2", "c.USART2", latency=L, baud=460_800),
        ],
    )
    try:
        tap = system.tap("a.USART1")
        tried: dict[str, list[str]] = {"a": [], "b": [], "c": []}

        def attempt(name: str, occurrences: tuple[int, int]) -> Callable[[Call], Return]:
            def handler(call: Call) -> Return:
                if call.occurrence in occurrences:
                    first = call.occurrence == occurrences[0]
                    tried[name].append(outcome(tap.break_line if first else tap.restore_line))
                return call.proceed()

            return handler

        a.intercept("wait_complete", attempt("a", (0, 1)))
        b.intercept("USART1_IRQHandler", attempt("b", (5, 6)))
        c.intercept("wait_complete", attempt("c", (0, 1)))

        async def sampled() -> str:
            return await a.sample(lambda: outcome(tap.break_line))

        assert asyncio.run(sampled()).startswith("MachineRunning")
        asyncio.run(to(system, at(3000)))
        assert tried["a"] == ["ok", "ok"]
        for name in ("b", "c"):
            assert len(tried[name]) == 2
            assert all(line.startswith("RuntimeError") for line in tried[name])
            assert all("only callbacks of a," in line for line in tried[name])
        assert any(f.cut for f in tap.frames())
    finally:
        system.close()


def callback_cuts(system: System) -> tuple[list[str], Outcome]:
    """From a's first wait_complete call: a cut 1 ms after its time, one before it; from b's
    5th interrupt a cut: what each did, a's first."""
    a, b = system.members["a"], system.members["b"]
    tap = system.tap("a.USART1")
    tried: list[str] = []
    by_b: list[str] = []

    def on_a(call: Call) -> Return:
        if call.occurrence == 0:
            own = system.to_system("a", a.time)
            start = VirtualTime(own.ps + ms(1).ps)
            tried.append(outcome(lambda: tap.cut(start, VirtualTime(start.ps + us(300).ps))))
            tried.append(f"{start.ps}")
            early = VirtualTime(own.ps - 1)
            tried.append(outcome(lambda: tap.cut(early, VirtualTime(own.ps + us(10).ps))))
            tried.append(f"{own.ps}")
        return call.proceed()

    def on_b(call: Call) -> Return:
        if call.occurrence == 5:
            by_b.append(outcome(lambda: tap.cut(at(2500), at(2600))))
        return call.proceed()

    a.intercept("wait_complete", on_a)
    b.intercept("USART1_IRQHandler", on_b)
    done = finish(system)
    return tried + by_b, done


@pytest.mark.parametrize(("slice_instructions", "third"), VARIANTS)
def test_a_cut_from_the_owner_s_callback_is_bound_by_its_time(
    pair: Build, slice_instructions: int, third: VirtualDuration | None
) -> None:
    tried, done = callback_cuts(pair(slice_instructions, third))
    reference, _ = callback_cuts(pair())
    assert tried == reference
    accepted, start_ps, early, own_ps, other = tried
    assert accepted == "ok"
    start = VirtualTime(int(start_ps))
    assert done == scheduled(pair, (1_000_000, None), start, VirtualTime(start.ps + us(300).ps))
    assert early.startswith("ValueError: cut [")
    assert f"starts before t={own_ps} ps, the earliest start accepted now" in early
    assert other.startswith("RuntimeError: cut from a callback: only callbacks of a")


@pytest.mark.parametrize("swap", [False, True], ids=["a stops", "b stops"])
def test_a_stopped_break_takes_the_later_end_s_system_time(pair: Build, swap: bool) -> None:
    entries = {"a_entry": "link_rx", "b_entry": "link_tx"} if swap else {}
    system = pair(**entries)
    stop = asyncio.run(system.run(until=at(3000)))
    assert stop.reason is not StopReason.TIME
    assert stop.member == ("b" if swap else "a")
    tap = system.tap("a.USART1")
    later = max(system.time_of("a"), system.time_of("b"))
    assert system.time_of("a") != system.time_of("b")
    assert tap.break_line() == later
    assert tap.restore_line() == later
    with pytest.raises(ValueError, match="no cut is open"):
        tap.restore_line()
    asyncio.run(to(system, at(2990.5)))
    assert system.time != at(2990.5)
    assert tap.break_line() == at(2990.5)


def snapshot_run(system: System, restored: popcorn.SystemSnapshot | None) -> Outcome:
    tap = system.tap("a.USART1")
    if restored is not None:
        system.restore(restored)
    asyncio.run(to(system, at(1000)))
    assert tap.restore_line() == at(1000)
    return finish(system)


def test_an_open_cut_is_in_a_snapshot_and_the_owner_is_kept_across_a_restore(
    pair: Build,
) -> None:
    system = pair()
    tap = system.tap("a.USART1")
    asyncio.run(to(system, at(500)))
    assert tap.break_line() == at(500)
    saved = system.snapshot()
    tap.frames()
    first = snapshot_run(system, None)
    tap.owner = "b"
    again = snapshot_run(system, saved)
    assert again == first
    assert tap.owner == "b"
    frames, _ = first
    assert [f.index for f in frames if f.cut] == list(range(8, 30))


def test_a_cut_window_stays_in_system_time_across_a_receiver_reset(pair: Build) -> None:
    def run(reset: bool) -> tuple[list[tuple[int, bool, bool]], VirtualTime]:
        system = pair()
        tap = system.tap("a.USART1")
        tap.cut(*WINDOW)
        asyncio.run(to(system, at(700)))
        if reset:
            system.reset("b", ResetKind.SOFTWARE)
            b = system.members["b"]
            b.reg.pc = b.sym("link_rx")
        tap.cut(at(1200), at(1300))
        asyncio.run(to(system, at(3000)))
        return [(f.index, f.cut, f.lost) for f in tap.frames()], system.to_local("b", at(1200))

    plain, plain_local = run(False)
    reset, reset_local = run(True)
    assert reset_local != plain_local
    assert reset == plain
    assert [index for index, cut, _ in plain if cut] == [*range(12, 32), 37, 38, 39]


def owner_after_resets(pair: Build, until_complete: bool) -> tuple[Seen, list[TapFrame]]:
    system = pair()
    a = system.members["a"]
    asyncio.run(to(system, at(100)))
    system.reset("a", ResetKind.POWER_ON)
    a.reg.pc = a.sym("idle")
    asyncio.run(to(system, at(200)))
    system.reset("a", ResetKind.SOFTWARE)
    a.reg.pc = a.sym("link_tx")
    tap = system.tap("a.USART1")
    seen: Seen = []
    if until_complete:

        async def stop_there() -> None:
            stop = await a.run(until="wait_complete")
            assert stop.reason is StopReason.ADDRESS

        asyncio.run(stop_there())
        return [(system.time_of("a"), system.time_of("a"))], []

    def at_complete(call: Call) -> Return:
        if call.occurrence == 0:
            seen.append((tap.break_line(), system.to_system("a", a.time)))
        return call.proceed()

    a.intercept("wait_complete", at_complete)
    frames, _ = finish(system)
    return seen, [f for f in frames if f.towards == "b.USART1"]


def intercept_lag(u575: popcorn.PartDescription) -> int:
    """On a lone link_tx machine, the time an intercept of wait_complete sees minus the time a
    run to that address stops at, in ps."""
    intercepted = member(u575, "link_tx", "a")
    seen: list[int] = []

    def at_complete(call: Call) -> Return:
        seen.append(intercepted.time.ps)
        return call.proceed()

    intercepted.intercept("wait_complete", at_complete, once=True)
    asyncio.run(intercepted.run(for_=ms(1)))
    reached = member(u575, "link_tx", "a")
    asyncio.run(reached.run(until="wait_complete"))
    return seen[0] - reached.time.ps


def test_a_callback_s_time_is_the_owner_s_system_time_after_its_resets(
    u575: popcorn.PartDescription, pair: Build
) -> None:
    seen, frames = owner_after_resets(pair, False)
    reached, _ = owner_after_resets(pair, True)
    [(start, own)] = seen
    [(stopped_at, _)] = reached
    assert start == own
    assert start.ps - stopped_at.ps == intercept_lag(u575)
    assert start > at(200)
    before = [f for f in frames if f.start < start]
    after = [f for f in frames if f.start >= start]
    assert before and after
    assert not any(f.cut for f in before[:-1])
    assert all(f.cut for f in after)


def test_refused_changes_leave_the_link_usable(pair: Build) -> None:
    system = pair(b_entry="link_tx")
    tap = system.tap("a.USART1")
    asyncio.run(to(system, at(100)))
    later = max(system.time_of("a"), system.time_of("b"))
    with pytest.raises(ValueError, match=rf"starts before t={later.ps} ps"):
        tap.cut(at(99), at(200))
    with pytest.raises(ValueError, match="end must be after start"):
        tap.cut(at(900), at(900))
    with pytest.raises(TypeError, match="start must be a VirtualTime, not VirtualDuration"):
        tap.cut(cast(VirtualTime, ms(1)), at(900))
    with pytest.raises(TypeError, match="end must be a VirtualTime"):
        tap.cut(at(900), cast(VirtualTime, 1000))
    with pytest.raises(ValueError, match="line must be 'idle' or 'break', not 'low'"):
        tap.cut(at(900), at(950), line=cast(popcorn.LineState, "low"))
    with pytest.raises(TypeError, match="line must be a str"):
        tap.break_line(line=cast(popcorn.LineState, 1))
    with pytest.raises(ValueError, match="no cut is open"):
        tap.restore_line()
    tap.cut(at(590), at(800))
    with pytest.raises(ValueError, match=r"overlaps the cut \[t=590000000 ps, t=800000000 ps\)"):
        tap.cut(at(700), at(900))
    with pytest.raises(ValueError, match=r"overlaps the cut \[t=590000000 ps"):
        tap.break_line()
    with pytest.raises(TypeError, match="owner must be a str"):
        tap.owner = cast(str, 1)
    with pytest.raises(ValueError, match="'c' is no member"):
        tap.owner = "c"
    assert tap.owner == "a"
    asyncio.run(to(system, at(900)))
    opened = tap.break_line()
    assert opened == max(system.time_of("a"), system.time_of("b"))
    with pytest.raises(ValueError, match=rf"the cut from t={opened.ps} ps is open"):
        tap.break_line()
    with pytest.raises(ValueError, match=rf"overlaps the cut open from t={opened.ps} ps"):
        tap.cut(at(2000), at(2100))
    asyncio.run(to(system, at(1000)))
    assert tap.restore_line() == max(system.time_of("a"), system.time_of("b"))
    b = system.members["b"]

    async def free() -> None:
        async with system.running():
            await b.elapse(us(10))
            await in_flight(system)
            with pytest.raises(MachineRunning, match="no owner change while it runs"):
                tap.owner = "b"

    asyncio.run(free())
    assert tap.owner == "a"
    tap.cut(at(1900), at(2300))
    asyncio.run(to(system, at(3000)))
    frames = tap.frames()
    for towards in ("a.USART1", "b.USART1"):
        mine = [f for f in frames if f.towards == towards]
        assert [f.index for f in mine if f.cut] == [*range(12, 21), *range(24, 30), *range(61, 64)]


def test_a_full_link_refusal_names_the_native_limit(pair: Build) -> None:
    system = pair()
    tap = system.tap("a.USART1")
    first = at(1000).ps
    for index in range(65536):
        tap.cut(VirtualTime(first + 2000 * index), VirtualTime(first + 2000 * index + 1000))
    with pytest.raises(ValueError, match=r"^cut \[.*\): 65536 cuts wait on the link already$"):
        tap.cut(at(500), at(600))


def test_a_run_with_cuts_and_no_callback_enters_python_on_no_machine_thread(
    pair: Build,
) -> None:
    def entries(intercepted: bool) -> tuple[list[int], list[int]]:
        system = pair()
        tap = system.tap("a.USART1")
        tap.cut(*WINDOW)
        tap.cut(at(1100), at(1500), line="break")
        if intercepted:
            system.members["b"].intercept("USART1_IRQHandler", lambda call: call.proceed())
        members = list(system.members.values())
        before = [m._handle.gil_entries() for m in members]  # pyright: ignore[reportPrivateUsage]
        asyncio.run(to(system, at(3000)))
        after = [m._handle.gil_entries() for m in members]  # pyright: ignore[reportPrivateUsage]
        assert any(f.cut for f in tap.frames())
        return before, after

    before, after = entries(intercepted=False)
    assert after == before
    before, after = entries(intercepted=True)
    assert after[0] == before[0]
    assert after[1] > before[1]


def injected_cut_run(system: System, running: bool) -> Outcome:
    """Cut [3990, 4030) us, then inject b"xyz" towards b at 4000 us, stopped or from the free
    run asleep on IDLE: the receiver's line is cut over [4010, 4050) us."""
    asyncio.run(to(system, at(3000)))
    past_done(system)
    tap = system.tap("b.USART1")
    tap.frames()
    tap.cut(at(3990), at(4030))
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
    return tuple(tap.frames()), tuple(logged(b))


def test_injected_frames_are_cut_on_their_receiver_times_stopped_and_running(
    pair: Build,
) -> None:
    stopped = injected_cut_run(pair(baud=FIRMWARE_BAUD), False)
    assert injected_cut_run(pair(baud=FIRMWARE_BAUD), True) == stopped
    frames, log = stopped
    assert [(f.data, f.injected, f.cut, f.lost) for f in frames] == [
        (ord("x"), True, True, True),
        (ord("y"), True, True, False),
        (ord("z"), True, True, True),
    ]
    assert bytes_of(log) == TX_BYTES


@pytest.mark.parametrize("third", [None, us(3)], ids=["q=l", "q=3us"])
def test_frames_ending_at_the_start_or_starting_at_the_end_are_delivered(
    pair: Build, third: VirtualDuration | None
) -> None:
    plain, _ = scheduled(pair, (1_000_000, None), at(5000), at(6000))
    start, end = plain[11].end, plain[20].start
    frames, log = scheduled(pair, (1_000_000, third), start, end)
    assert [f.index for f in frames if f.cut] == list(range(12, 20))
    assert not any(f.lost for f in frames)
    assert bytes_of(log) == TX_BYTES[:12] + TX_BYTES[20:]
    assert (frames, log) == scheduled(pair, (1_000_000, None), start, end)


def test_a_closed_system_and_its_tap_are_collected_after_cuts(
    u575: popcorn.PartDescription,
) -> None:
    a = member(u575, "link_tx", "a")
    system = System({"a": a, "b": member(u575, "link_rx", "b")}, links(None))
    tap = system.tap("a.USART1")
    tap.cut(*WINDOW, line="break")
    asyncio.run(to(system, at(1100)))
    later = max(system.time_of("a"), system.time_of("b"))
    assert tap.break_line() == later
    system.close()
    held = weakref.ref(system), weakref.ref(tap)
    del system, tap
    gc.collect()
    assert held[0]() is None and held[1]() is None


def test_a_free_run_does_not_sleep_through_a_break_cut_ahead(pair: Build) -> None:
    def run(free: bool) -> tuple[int, ...]:
        system = pair()
        asyncio.run(to(system, at(3000)))
        past_done(system)
        b = system.members["b"]
        tap = system.tap("a.USART1")
        tap.cut(at(4000), at(4500), line="break")
        if free:
            count = b.mem.u32[b.sym("log_len")]

            async def go() -> None:
                async with asyncio.timeout(20), system.running():
                    await b.wait_for(b.ref.var("log_len").ge(count + 4))

            asyncio.run(go())
        asyncio.run(to(system, at(5000)))
        return tuple(logged(b))

    reference = run(False)
    assert run(True) == reference
    tail = reference[-6::2]
    assert tail == (FE_ENTRY, LINK_BYTE << 16, LINK_IDLE << 16)


def test_the_tap_of_a_running_system_refuses_owner_changes_from_its_callbacks(
    pair: Build,
) -> None:
    system = pair()
    a = system.members["a"]
    tap = system.tap("a.USART1")
    tried: list[str] = []

    def on_a(call: Call) -> Return:
        if call.occurrence == 0:
            tried.append(outcome(lambda: setattr(tap, "owner", "b")))
        return call.proceed()

    a.intercept("wait_complete", on_a)
    finish(system)
    assert len(tried) == 1 and tried[0].startswith("MachineRunning")
    assert tap.owner == "a"
