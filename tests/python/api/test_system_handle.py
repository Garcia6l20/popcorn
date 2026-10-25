"""The private `_core.SystemHandle` binding over the native linked system: members, wires,
resumes, goals, taps and the GIL and lock rules of its calls."""

from __future__ import annotations

import asyncio
import gc
import os
import subprocess
import sys
import threading
import time
import weakref
from collections.abc import Callable, Iterator
from dataclasses import dataclass
from typing import cast

import popcorn
import pytest
from popcorn import _core
from popcorn._host import awaited
from popcorn.units import ms, us

from .linked import LINK_BYTE, PING_BYTES, USART_LL, logged, member

pytestmark = pytest.mark.usefixtures("both_sync_modes")


LATENCY = us(20).ps


def spec(sender: int, receiver: int, unit: str, latency_ps: int = LATENCY) -> _core.WireSpec:
    one = _core.WireSpec()
    one.sender = sender
    one.receiver = receiver
    one.receiver_unit = unit
    one.latency_ps = latency_ps
    one.baud = 460_800
    return one


@dataclass
class Linked:
    handle: _core.SystemHandle
    a: popcorn.Machine
    b: popcorn.Machine


def link(
    a: popcorn.Machine,
    b: popcorn.Machine,
    *,
    slice_instructions: int = 1_000_000,
    third: int | None = None,
) -> Linked:
    """`a` and `b` linked USART1 to USART1 both ways at 20 us, plus a USART2 pair at `third`
    picoseconds to lower the quantum."""
    handle = _core.SystemHandle(slice_instructions, 1_000_000)
    assert handle.add_member(a._handle, a.name, False) == 0  # pyright: ignore[reportPrivateUsage]
    assert handle.add_member(b._handle, b.name, False) == 1  # pyright: ignore[reportPrivateUsage]
    pairs = [("USART1", LATENCY)]
    if third is not None:
        pairs.append(("USART2", third))
    for unit, latency in pairs:
        a_link = a._ports.link(unit)  # pyright: ignore[reportPrivateUsage]
        b_link = b._ports.link(unit)  # pyright: ignore[reportPrivateUsage]
        handle.add_wire(spec(0, 1, unit, latency), a_link, b_link)
        handle.add_wire(spec(1, 0, unit, latency), b_link, a_link)
    handle.start([0, 0])
    return Linked(handle, a, b)


def resume(handle: _core.SystemHandle, until_ps: int | None) -> tuple[object, ...]:
    return cast(tuple[object, ...], handle.resume(until_ps).result(30.0))


def taken(box: list[_core.HostFuture]) -> object:
    """The result of the only future in `box`, which no frame keeps: a failed HostFuture holds
    its error natively, so a frame holding the future while the error's traceback holds the
    frame is a cycle the collector cannot see."""
    future = box.pop()
    try:
        return future.result(1.0)
    finally:
        del future


def run_to(linked: Linked, until_ps: int) -> list[tuple[str, int | None, int]]:
    """Resume to `until_ps`, releasing every member whose stop was delivered, and return the
    pauses as (reason, member, at_ps)."""
    pauses: list[tuple[str, int | None, int]] = []
    while True:
        reason, member_index, at_ps, _, error, _, _ = resume(linked.handle, until_ps)
        assert error is None, error
        paused = cast(_core.PauseReason, reason)
        pauses.append((paused.name, cast(int | None, member_index), cast(int, at_ps)))
        if paused is not _core.PauseReason.delivered:
            return pauses
        linked.handle.release(cast(int, member_index), True)


@pytest.fixture
def ping(u575: popcorn.PartDescription) -> Iterator[Callable[[int, int | None], Linked]]:
    made: list[Linked] = []

    def make(slice_instructions: int, third: int | None) -> Linked:
        a = member(u575, "link_ping", "a", slice_instructions)
        b = member(u575, "link_echo", "b", slice_instructions)
        linked = link(a, b, slice_instructions=slice_instructions, third=third)
        made.append(linked)
        return linked

    yield make
    for one in made:
        one.handle.close()


def outcome(linked: Linked) -> tuple[object, ...]:
    taps = tuple(
        tuple(
            (r.index, r.data, r.start_ps, r.end_ps, r.arrival_ps, r.lost)
            for r in linked.handle.tap_take(wire)
        )
        for wire in range(2)
    )
    return (
        tuple(logged(linked.a)),
        tuple(logged(linked.b)),
        linked.handle.time_of_ps(0),
        linked.handle.time_of_ps(1),
        linked.a.pc,
        taps,
    )


def test_two_members_ping_and_echo_with_the_same_result_at_any_slicing_or_quantum(
    ping: Callable[[int, int | None], Linked],
) -> None:
    results: list[tuple[object, ...]] = []
    for slice_instructions, third in [(1_000_000, None), (997, None), (1_000_000, us(3).ps)]:
        linked = ping(slice_instructions, third)
        assert linked.handle.quantum_ps() == (LATENCY if third is None else third)
        pauses = run_to(linked, ms(3).ps)
        assert pauses[0][:2] == ("delivered", 0)
        assert pauses[-1][0] == "system_time"
        results.append(outcome(linked))
    a_log = cast(tuple[int, ...], results[0][0])
    received = [word & 0xFFFF for word in a_log[::2] if word >> 16 == LINK_BYTE]
    assert received == [0x40 + index for index in range(PING_BYTES)]
    assert results[1] == results[0]
    assert results[2] == results[0]


def test_a_goal_stops_at_its_point_whatever_the_slicing_or_quantum(
    ping: Callable[[int, int | None], Linked],
) -> None:
    seen: list[tuple[int, int, int]] = []
    for slice_instructions, third in [
        (1_000_000, None),
        (997, None),
        (1_000_000, us(3).ps),
        (997, us(3).ps),
    ]:
        linked = ping(slice_instructions, third)
        target = linked.a.sym("USART1_IRQHandler")
        future = linked.handle.post_goal(0, target, None, None, None, None, None, False, None, None)
        pause = resume(linked.handle, ms(3).ps)
        assert cast(_core.PauseReason, pause[0]) is _core.PauseReason.delivered
        stop, at_ps = cast(tuple[_core.StopInfo, int], future.result(5.0))
        assert stop.reason is _core.StopReason.address
        assert stop.where.pc == target
        assert at_ps == pause[2]
        assert linked.handle.undecided() == [0]
        seen.append((at_ps, stop.instructions, linked.handle.time_of_ps(0)))
    assert seen[1:] == seen[:1] * 3


def test_a_member_with_a_pending_goal_refuses_sync_access_while_paused(
    ping: Callable[[int, int | None], Linked],
) -> None:
    linked = ping(1_000_000, None)
    future = linked.handle.post_goal(0, None, None, None, ms(5).ps, None, None, False, None, None)
    with pytest.raises(popcorn.MachineRunning, match="pending goal"):
        _ = linked.a.pc
    with pytest.raises(popcorn.MachineRunning, match="pending goal"):
        linked.a.mem.u32[linked.a.sym("log_len")] = 0
    assert linked.b.pc == linked.b.sym("link_echo")
    linked.handle.withdraw(0)
    assert linked.a.pc == linked.a.sym("link_ping")
    del future
    target = linked.a.sym("USART1_IRQHandler")
    future = linked.handle.post_goal(0, target, None, None, None, None, None, False, None, None)
    with pytest.raises(popcorn.MachineRunning, match="pending goal"):
        _ = linked.a.pc
    assert (
        cast(_core.PauseReason, resume(linked.handle, ms(3).ps)[0]) is _core.PauseReason.delivered
    )
    assert future.done()
    assert linked.a.pc == target
    linked.handle.finish()
    linked.handle.close()


def test_a_finish_clears_the_access_refusal_of_a_pending_goal(
    ping: Callable[[int, int | None], Linked],
) -> None:
    linked = ping(1_000_000, None)
    future = linked.handle.post_goal(1, None, None, None, ms(5).ps, None, None, False, None, None)
    with pytest.raises(popcorn.MachineRunning, match="pending goal"):
        _ = linked.b.pc
    linked.handle.finish()
    assert linked.b.pc == linked.b.sym("link_echo")
    assert future.done()


def test_member_indices_are_checked_before_anything_is_posted(
    ping: Callable[[int, int | None], Linked],
) -> None:
    linked = ping(1_000_000, None)
    calls: list[Callable[[], object]] = [
        lambda: linked.handle.post_goal(9, None, None, None, None, None, None, False, None, None),
        lambda: linked.handle.withdraw(9),
        lambda: linked.handle.release(9, True),
        lambda: linked.handle.reset(9, _core.ResetKind.power_on),
        lambda: linked.handle.time_of_ps(9),
    ]
    for call in calls:
        with pytest.raises(IndexError, match="no such member"):
            call()
    assert linked.handle.undecided() == []


def test_a_running_system_refuses_member_times_and_paused_injections(
    ping: Callable[[int, int | None], Linked],
) -> None:
    linked = ping(1_000_000, None)
    pending = linked.handle.resume(ms(500).ps)
    with pytest.raises(popcorn.MachineRunning, match="running"):
        linked.handle.time_of_ps(0)
    with pytest.raises(popcorn.MachineRunning, match="running"):
        linked.handle.pending_barrier_ps()
    with pytest.raises(popcorn.MachineRunning, match="running"):
        linked.handle.inject(0, [1], ms(400).ps, True)
    linked.handle.request_pause()
    pause = cast(tuple[object, ...], pending.result(30.0))
    assert cast(_core.PauseReason, pause[0]) in (
        _core.PauseReason.requested,
        _core.PauseReason.delivered,
    )
    assert linked.handle.time_of_ps(1) >= 0


def test_a_member_runs_alone_only_once_the_system_is_closed(
    ping: Callable[[int, int | None], Linked],
) -> None:
    linked = ping(1_000_000, None)
    with pytest.raises(popcorn.MachineBusy):
        asyncio.run(linked.a.run(for_=us(10)))
    run_to(linked, ms(1).ps)
    b_before = logged(linked.b)
    posted = linked.handle.posted(0)
    linked.handle.close()
    linked.handle.close()
    with pytest.raises(popcorn.EmulationError, match="closed"):
        linked.handle.time_of_ps(0)
    linked.a.reg.pc = linked.a.sym("link_tx_count")
    stop = asyncio.run(linked.a.run(budget=100_000_000))
    assert stop.reason is popcorn.StopReason.USER, stop
    assert linked.a.periph.USART1.port.available >= 512
    assert logged(linked.b) == b_before
    assert linked.handle.posted(0) == posted


def test_wires_and_members_are_checked_before_start(u575: popcorn.PartDescription) -> None:
    a = member(u575, "link_ping", "a")
    b = member(u575, "link_echo", "b")
    handle = _core.SystemHandle(1_000_000, 1_000_000)
    with pytest.raises(RuntimeError, match="a member and a wire"):
        handle.start([])
    handle.add_member(a._handle, "a", False)  # pyright: ignore[reportPrivateUsage]
    handle.add_member(b._handle, "b", False)  # pyright: ignore[reportPrivateUsage]
    a_link = a._ports.link("USART1")  # pyright: ignore[reportPrivateUsage]
    b_link = b._ports.link("USART1")  # pyright: ignore[reportPrivateUsage]
    with pytest.raises(ValueError, match="member"):
        handle.add_wire(spec(0, 2, "USART1"), a_link, b_link)
    with pytest.raises(ValueError, match="latency"):
        handle.add_wire(spec(0, 1, "USART1", 0), a_link, b_link)
    handle.add_wire(spec(0, 1, "USART1"), a_link, b_link)
    with pytest.raises(RuntimeError, match="one offset per member"):
        handle.start([0])
    with pytest.raises(popcorn.ConfigError):
        _core.SystemHandle(0, 1)
    handle.start([0, 0])
    with pytest.raises(RuntimeError, match="started already"):
        handle.start([0, 0])
    with pytest.raises(RuntimeError, match="before start"):
        handle.add_member(a._handle, "c", False)  # pyright: ignore[reportPrivateUsage]
    handle.close()


def test_the_lookahead_and_progress_are_checked_and_set_only_before_start(
    u575: popcorn.PartDescription,
) -> None:
    handle = _core.SystemHandle(1_000_000, 1_000_000)
    with pytest.raises(ValueError, match="progress -1 ps is below 0"):
        handle.set_progress(-1)
    with pytest.raises(ValueError, match="lookahead 0 ps is not above 0"):
        handle.set_lookahead(0)
    handle.set_progress(0)
    handle.set_lookahead(1)
    handle.close()
    linked = link(member(u575, "link_ping", "a"), member(u575, "link_echo", "b"))
    with pytest.raises(RuntimeError, match="the progress is set before start"):
        linked.handle.set_progress(25_000_000)
    with pytest.raises(RuntimeError, match="the lookahead is set before start"):
        linked.handle.set_lookahead(1_000_000)
    linked.handle.close()


def test_a_completion_callback_re_entering_the_system_raises_instead_of_deadlocking(
    ping: Callable[[int, int | None], Linked], monkeypatch: pytest.MonkeyPatch
) -> None:
    linked = ping(1_000_000, None)
    future = linked.handle.post_goal(0, None, None, None, ms(5).ps, None, None, False, None, None)
    raised: list[BaseException | None] = []

    def report(args: sys.UnraisableHookArgs) -> None:
        raised.append(args.exc_value)

    monkeypatch.setattr(sys, "unraisablehook", report)
    seen: list[int] = []

    def again() -> None:
        seen.append(len(linked.handle.undecided()))

    future.add_done_callback(again)
    linked.handle.withdraw(0)
    assert seen == []
    assert len(raised) == 1
    assert isinstance(raised[0], RuntimeError)
    assert "completes a request" in str(raised[0])
    box = [future]
    del future
    with pytest.raises(popcorn.EmulationError):
        taken(box)
    assert linked.handle.undecided() == []


def test_a_stop_when_calling_the_system_fails_the_goal(
    ping: Callable[[int, int | None], Linked],
) -> None:
    linked = ping(1_000_000, None)
    calls: list[int] = []

    def ask() -> bool:
        calls.append(1)
        linked.handle.request_pause()
        return False

    future = linked.handle.post_goal(0, None, None, None, ms(1).ps, ask, None, False, None, None)
    pause = resume(linked.handle, None)
    assert calls
    assert cast(_core.PauseReason, pause[0]) is _core.PauseReason.error
    assert isinstance(pause[4], RuntimeError)
    assert "callback of a machine" in str(pause[4])
    box = [future]
    del future
    with pytest.raises(RuntimeError, match="callback of a machine"):
        taken(box)


def test_a_stop_when_goal_takes_the_gil_only_for_its_calls(
    ping: Callable[[int, int | None], Linked],
) -> None:
    linked = ping(1_000_000, None)
    calls: list[int] = []

    def never() -> bool:
        calls.append(1)
        return False

    a_handle = linked.a._handle  # pyright: ignore[reportPrivateUsage]
    b_handle = linked.b._handle  # pyright: ignore[reportPrivateUsage]
    before = a_handle.gil_entries()
    b_before = b_handle.gil_entries()
    future = linked.handle.post_goal(0, None, None, None, ms(2).ps, never, None, False, None, None)
    pause = resume(linked.handle, None)
    assert cast(_core.PauseReason, pause[0]) is _core.PauseReason.delivered
    future.result(1.0)
    del future
    gc.collect()
    entries = a_handle.gil_entries() - before
    assert calls
    assert entries == len(calls)
    assert b_handle.gil_entries() == b_before


def test_the_handle_holds_its_members_until_closed(u575: popcorn.PartDescription) -> None:
    gc.collect()
    threads = _core.host_threads()
    a = member(u575, "link_ping", "a")
    b = member(u575, "link_echo", "b")
    handle = link(a, b).handle
    machine = weakref.ref(a)
    del a, b
    gc.collect()
    assert machine() is None
    assert _core.host_threads() == threads + 2
    handle.close()
    del handle
    gc.collect()
    assert _core.host_threads() == threads


def test_an_async_resume_completes_through_the_loop(
    ping: Callable[[int, int | None], Linked],
) -> None:
    linked = ping(1_000_000, None)

    async def body() -> tuple[object, ...]:
        return await awaited(
            linked.handle.resume(ms(1).ps), lambda raw: cast(tuple[object, ...], raw)
        )

    pause = asyncio.run(body())
    assert cast(_core.PauseReason, pause[0]) in (
        _core.PauseReason.delivered,
        _core.PauseReason.system_time,
    )


def test_an_injection_while_paused_is_checked_against_the_receiver(
    ping: Callable[[int, int | None], Linked],
) -> None:
    linked = ping(1_000_000, None)
    run_to(linked, ms(3).ps)
    linked.handle.tap_take(0)
    now = linked.handle.time_of_ps(1)
    refused = linked.handle.inject(0, [0x31], now - us(1).ps, True)
    assert refused == ("before_receiver", now)
    assert linked.handle.inject(0, [0x31, 0x32], now + us(50).ps, True) is None
    overlap = linked.handle.inject(0, [0x33], now + us(55).ps, True)
    assert overlap is not None and overlap[0] == "overlaps"
    run_to(linked, ms(4).ps)
    injected = [r for r in linked.handle.tap_take(0) if r.injected]
    assert [(r.data, r.start_ps) for r in injected][:1] == [(0x31, now + us(50).ps)]
    assert len(injected) == 2
    assert linked.handle.tap_dropped(0) == 0


def test_an_injection_while_running_is_refused_before_the_drained_reach(
    ping: Callable[[int, int | None], Linked],
) -> None:
    linked = ping(1_000_000, None)
    run_to(linked, ms(1).ps)
    refused = linked.handle.inject(0, [0x31], 0, False)
    assert refused is not None and refused[0] == "before_drained"


def test_a_replaced_frame_reaches_the_receiver_with_its_new_data(
    ping: Callable[[int, int | None], Linked],
) -> None:
    linked = ping(1_000_000, None)
    assert linked.handle.posted(0) == 0
    assert linked.handle.replace(0, 0, [0x99])
    assert linked.handle.drop(0, 1, 1)
    with pytest.raises(ValueError, match="at least one frame"):
        linked.handle.replace(0, 2, [])
    with pytest.raises(IndexError):
        linked.handle.posted(9)
    run_to(linked, ms(3).ps)
    assert linked.handle.posted(0) >= 2
    taps = linked.handle.tap_take(0)
    assert (taps[0].data, taps[1].dropped) == (0x99, True)
    received = [word & 0xFFFF for word in logged(linked.a)[::2] if word >> 16 == LINK_BYTE]
    assert received[0] == 0x99


def test_save_load_reset_and_finish_while_paused(ping: Callable[[int, int | None], Linked]) -> None:
    linked = ping(1_000_000, None)
    run_to(linked, ms(1).ps)
    state = linked.handle.save()
    assert len(state) > 0
    linked.handle.load(state)
    with pytest.raises(popcorn.ConfigError):
        linked.handle.load(b"not a state")
    before = linked.handle.time_of_ps(1)
    linked.handle.reset(1, _core.ResetKind.power_on)
    assert linked.handle.time_of_ps(1) == before
    assert linked.b.time.ps == 0
    linked.handle.finish()
    with pytest.raises(RuntimeError, match="finished"):
        linked.handle.resume(None)


def test_closing_from_another_thread_while_calls_run(u575: popcorn.PartDescription) -> None:
    a = member(u575, "link_ping", "a")
    b = member(u575, "link_echo", "b")
    outcomes: set[str] = set()
    for _ in range(100):
        handle = link(a, b).handle
        started = threading.Event()

        def hammer(handle: _core.SystemHandle = handle, started: threading.Event = started) -> None:
            started.set()
            while True:
                try:
                    handle.time_of_ps(0)
                    handle.pending_barrier_ps()
                    handle.request_pause()
                    handle.wake()
                    handle.inject(0, [1], 0, False)
                    outcomes.add("value")
                except popcorn.EmulationError:
                    outcomes.add("closed")
                    return

        thread = threading.Thread(target=hammer)
        thread.start()
        started.wait()
        handle.close()
        thread.join(30)
        assert not thread.is_alive()
    assert "closed" in outcomes


def test_a_system_collected_on_a_machine_thread_closes_off_that_thread(
    u575: popcorn.PartDescription,
) -> None:
    gc.collect()
    before = _core.host_threads()
    a = member(u575, "link_ping", "a")
    b = member(u575, "link_echo", "b")
    other = member(u575, "link_tx_count", "c")

    class Box:
        def __init__(self, handle: _core.SystemHandle) -> None:
            self.handle = handle
            self.itself = self

    box: list[Box] = [Box(link(a, b).handle)]
    a_link = weakref.ref(a)
    del a, b
    collected: list[int] = []

    def ask(_: popcorn.Machine) -> bool:
        if not collected:
            box.clear()
            collected.append(gc.collect())
        return True

    stop = asyncio.run(other.run(stop_when=ask, budget=10_000_000))
    assert stop.reason is popcorn.StopReason.USER
    assert collected and collected[0] > 0
    assert a_link() is None
    assert settles(lambda: _core.host_threads() == before + 1)
    del other
    gc.collect()
    assert settles(lambda: _core.host_threads() == before)


def settles(reached: Callable[[], bool], seconds: float = 10) -> bool:
    until = time.monotonic() + seconds
    while not reached():
        if time.monotonic() > until:
            return False
        time.sleep(0.001)
    return True


@pytest.mark.parametrize("closer_delay_ms", [None, 50])
def test_a_running_system_collected_in_a_member_s_callback_closes_off_that_thread(
    u575: popcorn.PartDescription,
    monkeypatch: pytest.MonkeyPatch,
    closer_delay_ms: int | None,
) -> None:
    if closer_delay_ms is not None:
        monkeypatch.setenv("POPCORN_TEST_CLOSER_DELAY_MS", str(closer_delay_ms))
    a = member(u575, "link_ping", "a")
    b = member(u575, "link_echo", "b")

    class Box:
        def __init__(self, handle: _core.SystemHandle) -> None:
            self.handle = handle
            self.itself = self

    box: list[Box] = [Box(link(a, b).handle)]
    collected: list[int] = []

    def ask() -> bool:
        if not collected:
            box.clear()
            collected.append(gc.collect())
        return False

    goal = box[0].handle.post_goal(0, None, None, None, ms(50).ps, ask, None, False, None, None)
    pending = box[0].handle.resume(ms(100).ps)
    pause = cast(tuple[object, ...], pending.result(30.0))
    assert collected and collected[0] > 0
    assert cast(_core.PauseReason, pause[0]) is _core.PauseReason.closed
    assert goal.done()
    del goal
    assert asyncio.run(a.run(for_=us(10))).reason is popcorn.StopReason.TIME


CHILD = """
import gc
import sys
import time

import popcorn
from popcorn import _core
from popcorn.stm32 import mcu

u575 = mcu("STM32U575QGIx")


def member(entry, name):
    m = popcorn.Machine(u575, image=popcorn.Image.elf({elf!r}), name=name)
    m.reset()
    import asyncio

    asyncio.run(m.run(until="main", budget=1_000_000))
    m.reg.pc = m.sym(entry)
    return m


a = member("link_ping", "a")
b = member("link_echo", "b")
handle = _core.SystemHandle(1_000_000, 1_000_000)
handle.add_member(a._handle, "a", False)
handle.add_member(b._handle, "b", False)
for sender, receiver, s_link, r_link in ((0, 1, a, b), (1, 0, b, a)):
    spec = _core.WireSpec()
    spec.sender, spec.receiver, spec.receiver_unit = sender, receiver, "USART1"
    spec.latency_ps, spec.baud = 20_000_000, 460_800
    handle.add_wire(spec, s_link._ports.link("USART1"), r_link._ports.link("USART1"))
{sync}
handle.start([0, 0])
{body}
print("ok")
"""

COLLECTED = """
class Box:
    def __init__(self, handle, members):
        self.handle = handle
        self.members = members
        self.itself = self


box = [Box(handle, [a, b])]
del handle, a, b
collected = []


def ask():
    if not collected:
        box.clear()
        collected.append(gc.collect())
    return False


def slow():
    time.sleep(0.3)
    return False


goal = box[0].handle.post_goal(0, None, None, None, 50_000_000_000, ask, None, False, None, None)
other = box[0].handle.post_goal(1, None, None, None, 50_000_000_000, slow, None, False, None, None)
pending = box[0].handle.resume(100_000_000_000)
while not collected:
    time.sleep(0.001)
del goal, other, pending
"""

BARRIER = 'handle.set_sync("barrier")'

BLOCKED = """
class Box:
    def __init__(self, handle, members):
        self.handle = handle
        self.members = members
        self.itself = self


box = [Box(handle, [a, b])]
del handle, a, b
collected = []
entered = []


def ask():
    while not entered:
        time.sleep(0.001)
    if not collected:
        box.clear()
        collected.append(gc.collect())
    return False


def stuck():
    entered.append(1)
    time.sleep(30)
    return False


goal = box[0].handle.post_goal(0, None, None, None, 50_000_000_000, ask, None, False, None, None)
other = box[0].handle.post_goal(1, None, None, None, 50_000_000_000, stuck, None, False, None, None)
pending = box[0].handle.resume(100_000_000_000)
while not collected:
    time.sleep(0.001)
del goal, other, pending
"""

LEFT_OPEN = """
pending = handle.resume(10_000_000_000_000)
time.sleep(0.05)
"""


@pytest.mark.parametrize("body", [COLLECTED, LEFT_OPEN], ids=["collected", "left open"])
def test_an_interpreter_exits_with_a_system_closing_or_open(
    u575: popcorn.PartDescription, body: str
) -> None:
    child = subprocess.run(
        [sys.executable, "-c", CHILD.format(elf=str(USART_LL), sync="", body=body)],
        capture_output=True,
        text=True,
        timeout=60,
        check=False,
    )
    assert child.returncode == 0, child.stderr
    assert child.stdout.strip() == "ok"
    assert child.stderr == ""


def test_an_exit_reports_a_closer_stuck_past_the_exit_budget(
    u575: popcorn.PartDescription,
) -> None:
    child = subprocess.run(
        [sys.executable, "-c", CHILD.format(elf=str(USART_LL), sync=BARRIER, body=BLOCKED)],
        capture_output=True,
        text=True,
        timeout=60,
        check=False,
        env={**os.environ, "POPCORN_TEST_EXIT_WAIT_MS": "1000"},
    )
    assert child.returncode == 0, child.stderr
    assert child.stdout.strip() == "ok"
    message = "a linked system closed by a machine's thread did not finish at exit"
    assert child.stderr.count(message) == 1, child.stderr
