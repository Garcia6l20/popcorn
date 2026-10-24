"""`System.reset`, `snapshot` and `restore`: member resets on the system's clock and the
whole system's state taken and given back."""

from __future__ import annotations

import asyncio
import dataclasses
import gc
import weakref
from collections.abc import Callable, Iterator
from typing import Any, cast

import popcorn
import pytest
from popcorn import (
    ResetKind,
    StopReason,
    System,
    SystemStop,
    TapFrame,
    UartLink,
    _core,
)
from popcorn.units import VirtualDuration, VirtualTime, ms, us

from .linked import logged, member, received
from .snapshot_doc import bson

pytestmark = pytest.mark.usefixtures("both_sync_modes")

L = us(20)
VARIANTS: list[tuple[int, VirtualDuration | None]] = [
    (1_000_000, None),
    (997, None),
    (1_000_000, us(3)),
    (997, us(3)),
]

Build = Callable[..., System]


def links(third: VirtualDuration | None) -> list[UartLink]:
    made = [UartLink("a.USART1", "b.USART1", latency=L, baud=460_800)]
    if third is not None:
        made.append(UartLink("a.USART2", "b.USART2", latency=third, baud=460_800))
    return made


@pytest.fixture
def pair(u575: popcorn.PartDescription) -> Iterator[Build]:
    """Builds `a` running `a_entry` linked USART1 to USART1 with `b` running `b_entry`."""
    made: list[System] = []

    def build(
        slice_instructions: int = 1_000_000,
        third: VirtualDuration | None = None,
        *,
        a_entry: str = "link_ping",
        b_entry: str = "link_echo",
    ) -> System:
        a = member(u575, a_entry, "a", slice_instructions)
        b = member(u575, b_entry, "b", slice_instructions)
        system = System({"a": a, "b": b}, links(third), slice_instructions=slice_instructions)
        made.append(system)
        return system

    yield build
    for one in made:
        one.close()


def observed(system: System) -> tuple[object, ...]:
    """What a member shows: logs, pc, instructions, local and system time."""
    return tuple(
        (tuple(logged(m)), m.pc, m.instructions, m.time, system.time_of(name))
        for name, m in system.members.items()
    )


def encoded(system: System) -> tuple[object, ...]:
    """Each member's snapshot document but its generation, which a restore bumps, and the
    system's own state bytes."""
    taken = system.snapshot()
    docs: list[object] = []
    for one in taken.members.values():
        doc = bson(one.to_bytes())
        doc.pop("generation")
        docs.append(doc)
    return (*docs, taken.state)


def stops_of(stops: list[SystemStop]) -> list[tuple[object, ...]]:
    return [(one.reason, one.member, one.time) for one in stops]


async def run_for(system: System, for_: VirtualDuration) -> list[SystemStop]:
    """`run(for_=)` again after each member stop, until the TIME stop: each counts from the
    same last target."""
    stops = [await system.run(for_=for_)]
    while stops[-1].reason is not StopReason.TIME:
        stops.append(await system.run(for_=for_))
    return stops


def frames(system: System) -> list[TapFrame]:
    return system.tap("a.USART1").frames()


@pytest.mark.parametrize(
    ("a_entry", "b_entry"), [("link_ping", "link_echo"), ("link_tx_count", "link_rx")]
)
def test_a_restore_then_a_run_equals_the_run_after_the_snapshot(
    pair: Build, a_entry: str, b_entry: str
) -> None:
    seen: list[tuple[object, ...]] = []
    for slice_instructions, third in VARIANTS:
        system = pair(slice_instructions, third, a_entry=a_entry, b_entry=b_entry)
        frames(system)
        first = asyncio.run(system.run(until=VirtualTime(ms(1.5).ps)))
        assert first.reason is StopReason.TIME
        before = frames(system)
        taken = system.snapshot()
        assert taken.target == VirtualTime(ms(1.5).ps)
        once = stops_of(asyncio.run(run_for(system, ms(2.5))))
        after = (observed(system), frames(system), encoded(system))
        system.restore(taken)
        again = stops_of(asyncio.run(run_for(system, ms(2.5))))
        assert again == once
        assert (observed(system), frames(system), encoded(system)) == after
        assert all(one.end.ps <= ms(1.5).ps for one in before)
        assert once[-1] == (StopReason.TIME, None, VirtualTime(ms(4).ps))
        if a_entry == "link_ping":
            assert once[0][:2] == (StopReason.USER, "a")
            assert received(system.members["a"]) == [0x40 + index for index in range(16)]
        else:
            assert any(one.start.ps < ms(1.5).ps < one.end.ps for one in after[1])
        seen.append((once, after[0], after[1]))
    assert seen[1:] == seen[:1] * 3


@pytest.mark.parametrize("created", [True, False])
def test_a_restore_drops_what_every_tap_held_created_or_not(pair: Build, created: bool) -> None:
    system = pair(a_entry="link_tx_count", b_entry="link_rx")
    asyncio.run(system.run(until=VirtualTime(ms(1.5).ps)))
    taken = system.snapshot()
    asyncio.run(system.run(until=VirtualTime(ms(2).ps)))
    if created:
        assert system.tap("a.USART1").available > 0
    system.restore(taken)
    asyncio.run(system.run(until=VirtualTime(ms(2).ps)))
    held = frames(system)
    assert held
    assert all(one.end.ps > ms(1.5).ps for one in held)
    keys = [(one.towards, one.index) for one in held]
    assert len(keys) == len(set(keys))


@pytest.mark.parametrize("held_by", ["run", "task"])
def test_a_snapshot_at_a_member_s_stop_releases_it_at_the_same_point(
    pair: Build, held_by: str
) -> None:
    def chain(snapshot: bool) -> list[tuple[object, ...]]:
        seen: list[tuple[object, ...]] = []
        for slice_instructions, third in VARIANTS:
            system = pair(slice_instructions, third)
            a = system.members["a"]

            async def body(system: System = system, a: popcorn.Machine = a) -> object:
                if held_by == "run":
                    stop = await system.run(for_=ms(3))
                    assert (stop.reason, stop.member) == (StopReason.USER, "a")
                else:
                    assert (await a.run(for_=us(700))).reason is StopReason.TIME
                if not snapshot:
                    return await a.run(for_=us(300))
                taken = system.snapshot()
                first = await a.run(for_=us(300))
                once = observed(system)
                system.restore(taken)
                second = await a.run(for_=us(300))
                assert observed(system) == once
                assert second == first
                return first

            result = asyncio.run(body())
            seen.append((result, observed(system)))
        if held_by == "task":
            assert seen[1:] == seen[:1] * 3
        return seen

    with_snapshot = chain(True)
    if held_by == "task":
        assert with_snapshot == chain(False)


def test_snapshot_and_restore_refusals(pair: Build) -> None:
    system = pair()
    a, b = system.members["a"], system.members["b"]
    taken = system.snapshot()
    with pytest.raises(TypeError, match="SystemSnapshot"):
        system.restore(cast(Any, taken.members))
    with pytest.raises(ValueError, match="members"):
        system.restore(dataclasses.replace(taken, members={"a": taken.members["a"]}))

    async def body() -> None:
        task = asyncio.create_task(system.run(for_=ms(50)))
        await asyncio.sleep(0)
        with pytest.raises(popcorn.MachineRunning, match="runs"):
            system.snapshot()
        with pytest.raises(popcorn.MachineRunning, match="runs"):
            system.restore(taken)
        with pytest.raises(popcorn.MachineRunning, match="runs"):
            system.reset("a")
        await task
        goal = asyncio.create_task(b.run(for_=ms(50)))
        await asyncio.sleep(0)
        with pytest.raises(popcorn.MachineRunning, match="goal of b"):
            system.snapshot()
        with pytest.raises(popcorn.MachineRunning, match="b has a goal pending"):
            b.reset()
        goal.cancel()
        with pytest.raises(asyncio.CancelledError):
            await goal
        async with a.running():
            with pytest.raises(popcorn.MachineRunning, match="a runs in its own running"):
                system.snapshot()
        async with system.running():
            with pytest.raises(popcorn.MachineRunning, match="runs"):
                system.restore(taken)

    asyncio.run(body())
    with pytest.raises(popcorn.MachineRunning, match="wait_for"):
        asyncio.run(system.wait_for(lambda one: one.restore(taken) is None, every=us(100)))
    with pytest.raises(KeyError):
        system.reset("c")
    with pytest.raises(TypeError, match="ResetKind"):
        system.reset("a", cast(Any, "power_on"))
    system.close()
    with pytest.raises(popcorn.EmulationError, match="closed"):
        system.snapshot()
    with pytest.raises(popcorn.EmulationError, match="closed"):
        system.restore(taken)
    with pytest.raises(popcorn.EmulationError, match="closed"):
        system.reset("a")


def test_a_snapshot_of_other_machines_restores_nothing(pair: Build) -> None:
    system = pair()
    other = pair()
    asyncio.run(system.run(until=VirtualTime(us(300).ps)))
    before = observed(system)
    with pytest.raises(popcorn.ConfigError, match="another machine"):
        system.restore(other.snapshot())
    junk = dataclasses.replace(system.snapshot(), state=b"not a state")
    asyncio.run(system.run(until=VirtualTime(us(600).ps)))
    moved = observed(system)
    assert moved != before
    port = system.members["a"].periph.USART1.port
    unread = port.available
    assert unread > 0
    with pytest.raises(popcorn.ConfigError):
        system.restore(junk)
    assert observed(system) == moved
    assert port.available == unread
    assert asyncio.run(system.run(until=VirtualTime(us(900).ps))).reason is StopReason.TIME


def test_a_member_stop_not_delivered_yet_holds_snapshots_back(
    u575: popcorn.PartDescription,
) -> None:
    x = member(u575, "tx_irq", "x")
    y = member(u575, "tx_irq", "y")
    link = UartLink("x.USART2", "y.USART2", latency=L, baud=460_800)
    with System({"x": x, "y": y}, [link]) as system:
        start = x.time
        taken = system.snapshot()
        first = asyncio.run(system.run(for_=ms(5)))
        assert (first.reason, first.member) == (StopReason.USER, "x")
        with pytest.raises(popcorn.MachineRunning, match="not delivered yet"):
            system.snapshot()
        with pytest.raises(popcorn.MachineRunning, match="not delivered yet"):
            system.restore(taken)
        second = asyncio.run(system.run(for_=ms(5)))
        assert (second.member, second.time) == ("y", first.time)
        system.snapshot()
        system.restore(taken)
        assert x.time == start


def test_a_reset_member_runs_as_a_lone_machine_reset_at_the_same_point(
    pair: Build, u575: popcorn.PartDescription
) -> None:
    reset_at = VirtualTime(us(500).ps)
    end = VirtualTime(ms(1.5).ps)
    seen: list[tuple[object, ...]] = []
    for slice_instructions, third in VARIANTS:
        system = pair(slice_instructions, third, a_entry="link_tx_count", b_entry="link_rx")
        a = system.members["a"]
        assert asyncio.run(system.run(until=reset_at)).reason is StopReason.TIME
        reset_local, kept = a.time, system.time_of("a")
        if third is None:
            a.reset()
        else:
            system.reset("a", ResetKind.POWER_ON)
        assert a.time == VirtualTime(0)
        assert system.time_of("a") == kept
        assert system.to_system("a", VirtualTime(0)) == kept
        assert asyncio.run(system.run(until=end)).reason is StopReason.TIME
        local_end = system.to_local("a", end)

        lone = member(u575, "link_tx_count", "lone", slice_instructions)
        assert (
            asyncio.run(lone.run(for_=VirtualDuration(reset_local.ps - lone.time.ps))).reason
            is StopReason.TIME
        )
        assert lone.time == reset_local
        lone.reset()
        assert asyncio.run(lone.run(for_=VirtualDuration(local_end.ps))).reason is StopReason.TIME
        assert (tuple(logged(a)), a.pc, a.instructions, a.time) == (
            tuple(logged(lone)),
            lone.pc,
            lone.instructions,
            lone.time,
        )
        towards_b = system.tap("a.USART1").frames(towards="b.USART1")
        lost = [one for one in towards_b if one.lost]
        assert len(lost) == 1 and lost[0].start.ps < kept.ps <= lost[0].end.ps
        seen.append((observed(system), towards_b, tuple(received(system.members["b"]))))
    assert seen[1:] == seen[:1] * 3


def test_a_peripheral_reset_kind_is_refused_before_the_link_drops_a_frame(pair: Build) -> None:
    reset_at = VirtualTime(us(500).ps)
    end = VirtualTime(ms(1.5).ps)

    def run(refused: bool) -> tuple[object, ...]:
        system = pair(a_entry="link_tx_count", b_entry="link_rx")
        a = system.members["a"]
        assert asyncio.run(system.run(until=reset_at)).reason is StopReason.TIME
        paused = system.time_of("a").ps
        if refused:
            kept = (a.time, system.time_of("a"))
            with pytest.raises(popcorn.ConfigError):
                system.reset("a", ResetKind.PERIPHERAL)
            with pytest.raises(popcorn.ConfigError):
                a.reset(ResetKind.PERIPHERAL)
            assert (a.time, system.time_of("a")) == kept
        assert asyncio.run(system.run(until=end)).reason is StopReason.TIME
        frames = system.tap("a.USART1").frames(towards="b.USART1")
        assert not any(one.lost for one in frames)
        assert any(one.start.ps < paused <= one.end.ps for one in frames)
        return frames, tuple(logged(a)), tuple(received(system.members["b"]))

    assert run(True) == run(False)


def test_a_member_held_at_its_goal_s_stop_resets_and_goes_on(pair: Build) -> None:
    seen: list[tuple[object, ...]] = []
    for slice_instructions, third in VARIANTS:
        system = pair(slice_instructions, third)
        a = system.members["a"]

        async def body(system: System = system, a: popcorn.Machine = a) -> object:
            stop, _ = await asyncio.gather(
                a.run(for_=us(700)), system.members["b"].run(for_=us(900))
            )
            assert stop.reason is StopReason.TIME
            a.reset()
            assert a.time == VirtualTime(0)
            return await a.run(until="main")

        stop = asyncio.run(body())
        seen.append((stop, observed(system)))
    assert seen[1:] == seen[:1] * 3


def test_a_system_snapshot_holds_no_system(u575: popcorn.PartDescription) -> None:
    a = member(u575, "link_ping", "a")
    b = member(u575, "link_echo", "b")
    system = System({"a": a, "b": b}, links(None))
    asyncio.run(system.run(until=VirtualTime(us(300).ps)))
    entries = [m._handle.gil_entries() for m in (a, b)]  # pyright: ignore[reportPrivateUsage]
    taken = system.snapshot()
    system.reset("b")
    system.restore(taken)
    assert [
        m._handle.gil_entries() for m in (a, b)  # pyright: ignore[reportPrivateUsage]
    ] == entries
    dead = weakref.ref(system)
    system.close()
    del system
    gc.collect()
    assert dead() is None
    assert set(taken.members) == {"a", "b"}


def test_a_member_reset_after_its_reset_request_runs_on_as_a_lone_machine(
    pair: Build, u575: popcorn.PartDescription
) -> None:
    end = VirtualTime(ms(2.5).ps)
    seen: dict[str, list[tuple[object, ...]]] = {}
    for slice_instructions, third in VARIANTS:
        for reset in ("at once", "after a run", None):
            a = member(u575, "link_tx_reset", "a", slice_instructions)
            b = member(u575, "link_rx", "b", slice_instructions)
            with System(
                {"a": a, "b": b},
                links(third),
                on_member_reset="stop",
                slice_instructions=slice_instructions,
            ) as system:
                stop = asyncio.run(system.run(for_=ms(1)))
                assert (stop.reason, stop.member) == (StopReason.RESET_REQUEST, "a")
                requested = (a.time, a.pc, a.instructions)
                halted_logs = tuple(logged(a))
                if reset != "at once":
                    handle = system._handle  # pyright: ignore[reportPrivateUsage]
                    executed = handle.executed_of(0)
                    halted = asyncio.run(system.run(until=VirtualTime(ms(1.2).ps)))
                    assert halted.reason is StopReason.TIME
                    assert (a.pc, tuple(logged(a))) == (requested[1], halted_logs)
                    assert handle.executed_of(0) == executed
                if reset is None:
                    continue
                system.reset("a")
                done = asyncio.run(system.run(until=end))
                assert (done.reason, done.member) == (StopReason.USER, "a")
                lone = member(u575, "link_tx_reset", "lone", slice_instructions)
                lone_stop = asyncio.run(lone.run(for_=ms(1)))
                assert lone_stop.reason is StopReason.RESET_REQUEST
                assert (lone.time, lone.pc, lone.instructions) == requested
                lone.reset()
                assert asyncio.run(lone.run(for_=ms(1.5))) == done.stop
                shown = (tuple(logged(a)), a.pc, a.instructions, a.time)
                assert shown == (tuple(logged(lone)), lone.pc, lone.instructions, lone.time)
                assert a.instructions > 0
                seen.setdefault(reset, []).append((shown, done.time))
    assert len(seen) == 2
    for one in seen.values():
        assert one[1:] == one[:1] * 3


def test_a_reset_drops_a_member_stop_not_delivered_yet(u575: popcorn.PartDescription) -> None:
    x = member(u575, "tx_irq", "x")
    y = member(u575, "tx_irq", "y")
    link = UartLink("x.USART2", "y.USART2", latency=L, baud=460_800)
    with System({"x": x, "y": y}, [link]) as system:
        first = asyncio.run(system.run(for_=ms(5)))
        assert (first.reason, first.member) == (StopReason.USER, "x")
        stopped = (x.pc, tuple(logged(x)))
        system.reset("y")
        system.snapshot()
        second = asyncio.run(system.run(for_=ms(5)))
        assert (second.reason, second.member) == (StopReason.USER, "y")
        assert second.time.ps > first.time.ps + ms(1).ps
        assert (x.pc, tuple(logged(x))) == stopped


def test_a_restore_after_an_error_runs_as_the_run_after_the_snapshot(pair: Build) -> None:
    seen: list[tuple[object, ...]] = []
    for slice_instructions, third in VARIANTS:
        system = pair(slice_instructions, third)
        a = system.members["a"]
        asyncio.run(system.run(until=VirtualTime(us(500).ps)))
        frames(system)
        taken = system.snapshot()
        once = stops_of(asyncio.run(run_for(system, ms(3.5))))
        after = (observed(system), frames(system), encoded(system))
        system.restore(taken)

        def refuse(call: popcorn.Call) -> popcorn.Return:
            raise ValueError("refused")

        hook = a.intercept("send_byte", refuse)
        with pytest.raises(ValueError, match="refused"):
            asyncio.run(system.run(for_=ms(3.5)))
        hook.remove()
        with pytest.raises(popcorn.EmulationError, match="stopped on an error"):
            asyncio.run(system.run(for_=ms(1)))
        with pytest.raises(popcorn.EmulationError, match="stopped on an error"):
            system.snapshot()
        with pytest.raises(popcorn.EmulationError, match="stopped on an error"):
            system.reset("a")
        with pytest.raises(popcorn.EmulationError, match="stopped on an error"):
            system.tap("a.USART1").inject(b"x", at=VirtualTime(ms(5).ps), towards="b.USART1")
        system.restore(taken)
        again = stops_of(asyncio.run(run_for(system, ms(3.5))))
        assert again == once
        assert (observed(system), frames(system), encoded(system)) == after
        seen.append((once, after[0], after[1]))
    assert seen[1:] == seen[:1] * 3


class FailingLoad:
    """A system handle whose `load` raises, every other call passed through."""

    def __init__(self, handle: _core.SystemHandle) -> None:
        self._handle = handle

    def load(self, state: bytes) -> None:
        del state
        raise popcorn.ConfigError("refused load", key="state", expected="a state")

    def __getattr__(self, name: str) -> object:
        return getattr(self._handle, name)


def test_a_failed_restore_gives_every_member_its_state_back(
    pair: Build, monkeypatch: pytest.MonkeyPatch
) -> None:
    system = pair()
    a = system.members["a"]
    taken = system.snapshot()
    asyncio.run(system.run(until=VirtualTime(us(600).ps)))
    moved = observed(system)
    handle = system._handle  # pyright: ignore[reportPrivateUsage]
    failing_load = cast(_core.SystemHandle, FailingLoad(handle))
    system._handle = failing_load  # pyright: ignore[reportPrivateUsage]
    restore = popcorn.Machine._restore  # pyright: ignore[reportPrivateUsage]
    calls: list[popcorn.Machine] = []

    def failing(machine: popcorn.Machine, snapshot: popcorn.Snapshot) -> None:
        calls.append(machine)
        if machine is a and calls.count(a) == 2:
            raise ValueError("a refused its own state")
        restore(machine, snapshot)

    monkeypatch.setattr(popcorn.Machine, "_restore", failing)
    with pytest.raises(ValueError, match="a refused") as raised:
        system.restore(taken)
    assert isinstance(raised.value.__cause__, popcorn.ConfigError)
    assert [m.name for m in calls] == ["a", "b", "a", "b"]
    monkeypatch.undo()
    system._handle = handle  # pyright: ignore[reportPrivateUsage]
    assert observed(system)[1] == moved[1]


def test_a_restore_restarts_what_each_tap_dropped(
    pair: Build, monkeypatch: pytest.MonkeyPatch
) -> None:
    from popcorn import _system

    monkeypatch.setattr(_system, "_TAP_KEEP", 8)
    system = pair(a_entry="link_tx_count", b_entry="link_rx")
    tap = system.tap("a.USART1")
    taken = system.snapshot()
    asyncio.run(system.run(until=VirtualTime(ms(1).ps)))
    first = (tap.available, tap.dropped)
    assert first[1] > 0
    system.restore(taken)
    assert (tap.available, tap.dropped) == (0, 0)
    asyncio.run(system.run(until=VirtualTime(ms(1).ps)))
    assert (tap.available, tap.dropped) == first


def test_a_member_resumed_past_its_reset_request_keeps_no_reset_after_the_close(
    u575: popcorn.PartDescription,
) -> None:
    a = member(u575, "link_tx_reset", "a")
    b = member(u575, "link_rx", "b")
    with System({"a": a, "b": b}, links(None), on_member_reset="stop") as system:
        stop = asyncio.run(system.run(for_=ms(1)))
        assert (stop.reason, stop.member) == (StopReason.RESET_REQUEST, "a")
        requested = a.time
        resumed = asyncio.run(system.run(until=VirtualTime(ms(1.2).ps)))
        assert resumed.reason is StopReason.TIME
    assert stop.stop is not None
    alone = asyncio.run(a.run(budget=1_000))
    assert alone.resets == stop.stop.resets
    assert alone.time >= requested
