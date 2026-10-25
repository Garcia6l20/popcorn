"""Member goals of a `System`: a member's `run`, `step`, `wait_for` and `running()` run as
goals on the system's clock, park at their stop, and release by the code's structure."""

from __future__ import annotations

import asyncio
import contextlib
import gc
import logging
import threading
import time
import weakref
from collections.abc import Callable, Coroutine, Iterator
from typing import Any

import popcorn
import pytest
from popcorn import Stop, StopReason, System, UartLink, _core
from popcorn.units import VirtualDuration, VirtualTime, ms, s, us

from .linked import Ticker, logged, member, received, ticker

L = us(20)
IRQ = "USART1_IRQHandler"
VARIANTS: list[tuple[int, VirtualDuration | None]] = [
    (1_000_000, None),
    (997, None),
    (1_000_000, us(3)),
    (997, us(3)),
]

Build = Callable[..., System]


@pytest.fixture
def ping(u575: popcorn.PartDescription) -> Iterator[Build]:
    made: list[System] = []

    def build(
        slice_instructions: int = 1_000_000,
        third: VirtualDuration | None = None,
        **options: Any,
    ) -> System:
        a = member(u575, "link_ping", "a", slice_instructions)
        b = member(u575, "link_echo", "b", slice_instructions)
        links = [UartLink("a.USART1", "b.USART1", latency=L, baud=460_800)]
        if third is not None:
            links.append(UartLink("a.USART2", "b.USART2", latency=third, baud=460_800))
        system = System({"a": a, "b": b}, links, slice_instructions=slice_instructions, **options)
        made.append(system)
        return system

    yield build
    for one in made:
        one.close()


def across(ping: Build, body: Callable[[System], Coroutine[Any, Any, object]]) -> object:
    """`body`'s result on a fresh system in every variant, checked equal, and returned."""
    results: list[object] = []
    for slice_instructions, third in VARIANTS:
        system = ping(slice_instructions, third)
        results.append(asyncio.run(body(system)))
    assert results[1:] == results[:1] * (len(results) - 1)
    return results[0]


def point(stop: Stop) -> tuple[object, ...]:
    return (stop.reason, stop.pc, stop.instructions, stop.time)


ECHOED = ms(1)


def test_a_gather_of_member_goals_parks_each_at_its_stop_at_any_slicing(ping: Build) -> None:
    async def body(system: System) -> object:
        a, b = system.members["a"], system.members["b"]
        a_stop, b_stop = await asyncio.gather(a.run(until=IRQ), b.run(for_=ECHOED))
        assert a_stop.reason is StopReason.ADDRESS and b_stop.reason is StopReason.TIME
        assert a.pc == a.sym(IRQ)
        assert system.time_of("a") == system.time_of("b") == system.time_of("b")
        assert system.time_of("a").ps - a.time.ps == system.time_of("b").ps - b.time.ps + (
            system.time_of("a").ps - a.time.ps - system.time_of("b").ps + b.time.ps
        )
        return point(a_stop), point(b_stop), system.time_of("a"), system.time_of("b")

    across(ping, body)


def test_a_member_resolved_first_is_halted_up_to_the_last_one(ping: Build) -> None:
    async def body(system: System) -> object:
        a, b = system.members["a"], system.members["b"]
        a_stop, b_stop = await asyncio.gather(a.run(until=IRQ), b.run(for_=ECHOED))
        executed = a.instructions
        assert a.pc == a.sym(IRQ)
        assert system.time_of("a") == system.time_of("b")
        assert executed >= a_stop.instructions
        return point(a_stop), point(b_stop), system.time_of("a")

    across(ping, body)


def test_a_nested_re_issue_starts_exactly_at_the_first_stop(ping: Build) -> None:
    async def two(system: System, pause: bool) -> tuple[Stop, Stop]:
        a = system.members["a"]
        first = await a.run(until=IRQ)
        if pause:
            await asyncio.sleep(0.02)
        second = await a.step(5)
        return first, second

    async def body(system: System, pause: bool = False) -> object:
        b = system.members["b"]
        (first, second), b_stop = await asyncio.gather(two(system, pause), b.run(for_=ECHOED))
        assert second.reason is StopReason.BUDGET
        return point(first), point(second), point(b_stop)

    plain = across(ping, body)

    async def slept(system: System) -> object:
        return await body(system, True)

    assert across(ping, slept) == plain


def test_a_member_halted_after_its_stop_lets_the_gather_go_on(ping: Build) -> None:
    async def body(system: System) -> object:
        a, b = system.members["a"], system.members["b"]

        async def first() -> Stop:
            stop = await a.run(until=IRQ)
            a.halt()
            await asyncio.sleep(0.05)
            return stop

        a_stop, b_stop = await asyncio.gather(first(), b.run(for_=ECHOED))
        return point(a_stop), point(b_stop)

    across(ping, body)


def test_the_watchdog_names_a_member_kept_parked_by_a_foreign_wait(
    ping: Build, monkeypatch: pytest.MonkeyPatch, caplog: pytest.LogCaptureFixture
) -> None:
    from popcorn import _system

    monkeypatch.setattr(_system, "_WATCHDOG", 0.05)
    system = ping()
    a, b = system.members["a"], system.members["b"]

    async def body() -> None:
        done = asyncio.Event()

        async def first() -> None:
            await a.run(until=IRQ)
            asyncio.get_running_loop().call_later(0.3, a.halt)
            await done.wait()

        async def second() -> None:
            await b.run(for_=ECHOED)
            done.set()

        await asyncio.gather(first(), second())

    with caplog.at_level(logging.WARNING, logger="popcorn"):
        asyncio.run(body())
    warnings = [r for r in caplog.records if "waits at its goal's stop" in r.getMessage()]
    assert len(warnings) == 1
    assert warnings[0].getMessage().startswith("a waits")


def test_a_write_on_a_halted_member_lands_at_the_delivered_point(ping: Build) -> None:
    async def body(system: System) -> object:
        a, b = system.members["a"], system.members["b"]
        await b.run(for_=ECHOED)
        a.mem.u32[a.sym("log_len")] = 0
        at = system.time_of("a")
        await asyncio.gather(a.run(for_=ECHOED), b.run(for_=ECHOED))
        return at, tuple(logged(a)), received(a)

    across(ping, body)


def test_a_member_of_a_live_system_refuses_a_rename(ping: Build) -> None:
    system = ping()
    a = system.members["a"]
    with pytest.raises(popcorn.MachineBusy, match="a is a member of a linked system"):
        a._rename("z")  # pyright: ignore[reportPrivateUsage]
    assert a.name == "a"
    assert system.members["a"] is a


def test_sync_access_to_a_member_with_a_goal_raises(ping: Build) -> None:
    system = ping()
    a, b = system.members["a"], system.members["b"]
    seen: list[str] = []

    def peek(_: popcorn.Machine) -> bool:
        try:
            seen.append(hex(b.pc))
        except popcorn.MachineRunning as error:
            seen.append(str(error))
        return True

    async def body() -> None:
        task = asyncio.create_task(b.run(until=IRQ, budget=10_000_000))
        await asyncio.sleep(0)
        with pytest.raises(popcorn.MachineRunning):
            _ = b.pc
        with pytest.raises(popcorn.MachineBusy, match="one goal per member"):
            await b.run(until=IRQ)
        with pytest.raises(popcorn.MachineBusy, match="pending"):
            await system.run(for_=ms(1))
        await asyncio.gather(task, a.run(stop_when=peek, budget=10_000_000))

    asyncio.run(body())
    assert seen and all("pending goal" in one or "running" in one for one in seen)


def test_a_cancelled_goal_is_withdrawn_and_the_others_go_on(ping: Build) -> None:
    system = ping()
    a, b = system.members["a"], system.members["b"]

    async def body() -> tuple[Stop, Stop]:
        pinging = asyncio.create_task(a.run(until="idle", budget=10**12))
        reached = await b.run(until=IRQ, budget=10_000_000)
        pinging.cancel()
        with pytest.raises(asyncio.CancelledError):
            await pinging
        return reached, await b.run(for_=us(100))

    reached, after = asyncio.run(body())
    assert reached.reason is StopReason.ADDRESS
    assert after.reason is StopReason.TIME
    assert system.time_of("a").ps >= 0
    assert asyncio.run(system.run(for_=ms(1))).reason in (StopReason.TIME, StopReason.USER)


def test_a_member_running_body_runs_it_beside_another_goal(ping: Build) -> None:
    system = ping()
    a, b = system.members["a"], system.members["b"]

    async def body() -> Stop:
        async with a.running():
            return await b.run(until=IRQ, budget=10_000_000)

    assert asyncio.run(body()).reason is StopReason.ADDRESS
    assert b.pc == b.sym(IRQ)
    assert asyncio.run(a.step(1)).reason is StopReason.BUDGET


def test_a_member_stop_inside_running_raises_on_leaving(ping: Build) -> None:
    system = ping()
    a, b = system.members["a"], system.members["b"]

    async def body() -> None:
        async with a.running():
            await b.run(for_=ms(3))

    with pytest.raises(popcorn.EmulationError, match="USER"):
        asyncio.run(body())


def test_a_member_asleep_in_running_stands_at_a_member_stop_s_time(
    u575: popcorn.PartDescription,
) -> None:
    async def body(system: System) -> tuple[VirtualTime, VirtualTime]:
        a, b = system.members["a"], system.members["b"]
        async with a.running():
            stop = await b.run(for_=ms(1) + us(3))
            assert stop.reason is StopReason.TIME
            return system.time_of("a"), system.time_of("b")

    seen: list[tuple[VirtualTime, VirtualTime]] = []
    for latency in (L, VirtualDuration(L.ps // 3)):
        a, b = member(u575, "idle", "a"), member(u575, "idle", "b")
        links = [UartLink("a.USART1", "b.USART1", latency=latency, baud=460_800)]
        system = System({"a": a, "b": b}, links)
        try:
            seen.append(asyncio.run(body(system)))
        finally:
            system.close()
    assert seen[0][0] == seen[0][1]
    assert seen[1] == seen[0]


def test_a_goal_cap_stops_a_sleeping_member(ping: Build) -> None:
    system = ping(goal_cap=ms(1))
    b = system.members["b"]
    stop = asyncio.run(b.run(until="link_ping"))
    assert stop.reason is StopReason.TIME
    assert stop.detail == "goal cap"
    with pytest.raises(TypeError):
        ping(goal_cap=1000)


@pytest.mark.parametrize("cap", [us(0), VirtualDuration(-1)])
def test_a_goal_cap_not_above_0_is_refused(ping: Build, cap: VirtualDuration) -> None:
    with pytest.raises(ValueError, match="goal_cap must be above 0"):
        ping(goal_cap=cap)


@pytest.mark.parametrize(("slice_instructions", "third"), VARIANTS)
def test_a_goal_failed_by_a_halted_member_s_error_raises_it_naming_that_member(
    u575: popcorn.PartDescription, slice_instructions: int, third: VirtualDuration | None
) -> None:
    a = member(u575, "link_ping", "a", slice_instructions, models={"TIM6": Ticker})
    b = member(u575, "link_echo", "b", slice_instructions)
    links = [UartLink("a.USART1", "b.USART1", latency=L, baud=460_800)]
    if third is not None:
        links.append(UartLink("a.USART2", "b.USART2", latency=third, baud=460_800))
    ticker(a).raise_at = 150
    with System({"a": a, "b": b}, links, slice_instructions=slice_instructions):

        async def body() -> None:
            await a.run(for_=us(100))
            with pytest.raises(RuntimeError, match="TIM6 tick 150") as raised:
                await b.run(for_=ms(3))
            assert raised.value.__notes__ == ["raised by member a at system time t=1100500000 ps"]

        asyncio.run(body())


class Blocking(Ticker):
    """A Ticker that, at its raising tick, sets `entered` and waits for `go` first."""

    entered = threading.Event()
    go = threading.Event()

    def _tick(self) -> None:
        if self.ticks + 1 == self.raise_at:
            Blocking.entered.set()
            Blocking.go.wait(10)
        super()._tick()


@pytest.mark.parametrize(("slice_instructions", "third"), VARIANTS)
def test_leaving_a_member_s_running_raises_a_peer_s_error_landing_meanwhile(
    u575: popcorn.PartDescription, slice_instructions: int, third: VirtualDuration | None
) -> None:
    a = member(u575, "link_ping", "a", slice_instructions)
    b = member(u575, "link_echo", "b", slice_instructions, models={"TIM6": Blocking})
    links = [UartLink("a.USART1", "b.USART1", latency=L, baud=460_800)]
    if third is not None:
        links.append(UartLink("a.USART2", "b.USART2", latency=third, baud=460_800))
    ticker(b).raise_at = ticker(b).ticks + 5
    Blocking.entered.clear()
    Blocking.go.clear()
    with System({"a": a, "b": b}, links, slice_instructions=slice_instructions):

        async def body() -> None:
            async with asyncio.timeout(10):
                with pytest.raises(RuntimeError, match="TIM6 tick") as raised:
                    async with a.running():
                        while not Blocking.entered.is_set():
                            await asyncio.sleep(0.001)
                        asyncio.get_running_loop().call_soon(Blocking.go.set)
                assert raised.value.__notes__[0].startswith("raised by member b at system time")

        asyncio.run(body())


def test_a_stop_when_goal_takes_the_gil_once_per_call(ping: Build) -> None:
    system = ping()
    a, b = system.members["a"], system.members["b"]
    calls: list[int] = []

    def never(_: popcorn.Machine) -> bool:
        calls.append(1)
        return False

    a_before = a._handle.gil_entries()  # pyright: ignore[reportPrivateUsage]
    b_before = b._handle.gil_entries()  # pyright: ignore[reportPrivateUsage]
    stop = asyncio.run(a.run(stop_when=never, for_=ms(2)))
    gc.collect()
    assert stop.reason is StopReason.TIME
    assert calls
    assert a._handle.gil_entries() - a_before == len(calls)  # pyright: ignore[reportPrivateUsage]
    assert b._handle.gil_entries() == b_before  # pyright: ignore[reportPrivateUsage]


class Held:
    pass


def goal_path(u575: popcorn.PartDescription, path: str) -> list[weakref.ref[object]]:
    """Run one goal of `path` on a fresh system, close it, return weakrefs to what it held."""
    a = member(u575, "link_ping", "a")
    b = member(u575, "link_echo", "b")
    system = System({"a": a, "b": b}, [UartLink("a.USART1", "b.USART1", latency=L, baud=460_800)])
    held = Held()

    def predicate(_: popcorn.Machine) -> bool:
        if path == "raised":
            raise ValueError(held)
        return path == "success"

    async def body() -> None:
        if path == "withdrawn":
            with pytest.raises(TimeoutError):
                await asyncio.wait_for(
                    a.run(stop_when=predicate, until="idle", budget=10**12), 0.01
                )
        elif path == "closed":
            task = asyncio.create_task(a.run(stop_when=predicate, for_=ms(50)))
            await asyncio.sleep(0.01)
            system.close()
            with pytest.raises(popcorn.EmulationError):
                await task
        elif path == "raised":
            with pytest.raises(ValueError):
                await a.run(stop_when=predicate, for_=ms(1))
        else:
            await a.run(stop_when=predicate, for_=ms(1))

    asyncio.run(body())
    system.close()
    return [weakref.ref(held), weakref.ref(a), weakref.ref(system)]


@pytest.mark.parametrize("path", ["success", "raised", "withdrawn", "closed"])
def test_a_goal_s_predicate_and_members_are_freed_on_every_path(
    u575: popcorn.PartDescription, path: str
) -> None:
    refs = goal_path(u575, path)
    gc.collect()
    assert [one() for one in refs] == [None, None, None]


def test_many_withdrawn_goals_leave_no_entry(ping: Build) -> None:
    system = ping()
    a = system.members["a"]

    async def body() -> None:
        for _ in range(200):
            with pytest.raises(TimeoutError):
                await asyncio.wait_for(a.run(until="idle", budget=10**12), 0.001)

    asyncio.run(body())
    assert system._goals == {}  # pyright: ignore[reportPrivateUsage]
    assert system._undecided == {}  # pyright: ignore[reportPrivateUsage]
    assert system._withdrawals == set()  # pyright: ignore[reportPrivateUsage]


def test_awaiting_another_machine_releases_a_parked_member(ping: Build) -> None:
    system = ping()
    a, b = system.members["a"], system.members["b"]

    async def body() -> tuple[Stop, Stop]:
        echoed = asyncio.Event()

        async def first() -> Stop:
            stop = await a.run(until=IRQ)
            await b.now()
            await echoed.wait()
            return stop

        async def second() -> Stop:
            stop = await b.run(for_=ECHOED)
            echoed.set()
            return stop

        return await asyncio.wait_for(asyncio.gather(first(), second()), 10)

    a_stop, b_stop = asyncio.run(body())
    assert (a_stop.reason, b_stop.reason) == (StopReason.ADDRESS, StopReason.TIME)


def test_a_halt_and_a_goal_in_the_same_turn_start_together(ping: Build) -> None:
    async def body(system: System) -> object:
        a, b = system.members["a"], system.members["b"]

        async def seq() -> tuple[Stop, Stop]:
            first = await a.run(until=IRQ)
            a.halt()
            assert system._flight is None  # pyright: ignore[reportPrivateUsage]
            second = await a.step(5)
            return first, second

        (first, second), b_stop = await asyncio.gather(seq(), b.run(for_=ECHOED))
        return point(first), point(second), point(b_stop)

    across(ping, body)


@pytest.mark.parametrize("running", [False, True], ids=["paused", "running"])
def test_a_goal_delivered_after_its_task_was_cancelled_releases_its_member(
    ping: Build, running: bool
) -> None:
    system = ping()
    a, b = system.members["a"], system.members["b"]
    handle = system._handle  # pyright: ignore[reportPrivateUsage]

    async def body() -> Stop:
        peer = asyncio.create_task(b.run(for_=ECHOED)) if running else None
        task = asyncio.create_task(a.step(1))
        goal = None
        while goal is None:
            goal = system._goals.get(0)  # pyright: ignore[reportPrivateUsage]
            await asyncio.sleep(0)
        while not goal.done():
            await asyncio.sleep(0)
        while not running and system._flight is not None:  # pyright: ignore[reportPrivateUsage]
            await asyncio.sleep(0)
        task.cancel()
        with pytest.raises(asyncio.CancelledError):
            await task
        if peer is not None:
            return await asyncio.wait_for(peer, 10)
        return await asyncio.wait_for(b.run(for_=ECHOED), 10)

    assert asyncio.run(body()).reason is StopReason.TIME
    assert handle.undecided() == []
    assert system._undecided == {}  # pyright: ignore[reportPrivateUsage]


def running_path(u575: popcorn.PartDescription, path: str) -> list[weakref.ref[object]]:
    """A member's running() body ending by `path`, then the system closed: weakrefs to what the
    body held."""
    a = member(u575, "link_ping", "a")
    b = member(u575, "link_echo", "b")
    system = System({"a": a, "b": b}, [UartLink("a.USART1", "b.USART1", latency=L, baud=460_800)])
    held = Held()

    def refuse(call: popcorn.Call) -> popcorn.Return:
        raise ValueError(held)

    async def body() -> None:
        kept = held
        if path == "failed":
            a.intercept("send_byte", refuse)
            with pytest.raises(ValueError):
                async with a.running():
                    while system._failed is None:  # pyright: ignore[reportPrivateUsage]
                        await asyncio.sleep(0.001)
                    assert kept is held
        elif path == "closed":
            with pytest.raises(popcorn.EmulationError):
                async with a.running():
                    system.close()
                    await asyncio.sleep(0.01)
                    assert kept is held
        elif path == "raised":
            with pytest.raises(ValueError):
                async with a.running():
                    raise ValueError(kept)
        elif path == "stopped":
            with pytest.raises(popcorn.EmulationError, match="USER"):
                async with a.running():
                    await b.run(for_=ms(3))
                    assert kept is held
        else:
            async with a.running():
                await b.run(for_=us(100))
                assert kept is held

    asyncio.run(body())
    system.close()
    return [weakref.ref(held), weakref.ref(a), weakref.ref(system)]


@pytest.mark.parametrize("path", ["plain", "raised", "stopped", "closed", "failed"])
def test_a_member_running_body_frees_what_it_held(u575: popcorn.PartDescription, path: str) -> None:
    refs = running_path(u575, path)
    gc.collect()
    assert [one() for one in refs] == [None, None, None]


async def waits_in_running(m: popcorn.Machine) -> tuple[object, ...]:
    """A Condition wait and a stream read inside `m.running()`, the stop that ends the free run
    ignored."""
    out: list[object] = []
    with contextlib.suppress(popcorn.EmulationError):
        async with m.running():
            stop = await m.wait_for(m.ref.var("tx_next").ge(3))
            out.append((stop.reason, stop.time, stop.instructions))
            out.append(await m.periph.USART1.port.read(7))
    return tuple(out)


def test_waits_inside_a_member_s_running_equal_a_lone_machine_s(
    u575: popcorn.PartDescription,
) -> None:
    async def parked(m: popcorn.Machine) -> tuple[object, ...]:
        stop = await m.wait_for(m.ref.var("tx_next").ge(3))
        assert (await m.run()).reason is StopReason.USER
        data = await m.periph.USART1.port.read(7)
        return (stop.reason, stop.time, stop.instructions), data

    lone = asyncio.run(parked(member(u575, "tx_irq", "a", slice_wall_budget=60)))
    assert lone[1] == b"hello\r\n"
    for slice_instructions, third in VARIANTS:
        a = member(u575, "tx_irq", "a", slice_instructions)
        b = member(u575, "idle", "b", slice_instructions)
        links = [UartLink("a.USART1", "b.USART1", latency=L, baud=460_800)]
        if third is not None:
            links.append(UartLink("a.USART2", "b.USART2", latency=third, baud=460_800))
        with System({"a": a, "b": b}, links, slice_instructions=slice_instructions) as system:
            assert asyncio.run(waits_in_running(system.members["a"])) == lone


async def read_for(m: popcorn.Machine) -> tuple[object, ...]:
    """Every frame `m`'s USART1 sends in 500 us, a read that ends on its duration, and the
    reason of the member stop it ended at."""
    got = await m.periph.USART1.port.read_frames(100, for_=us(500))
    assert m.last_stop is not None
    return bytes(f.data for f in got), m.last_stop.reason


def test_a_member_read_that_ends_on_its_duration_takes_the_frames_sent_by_then(
    u575: popcorn.PartDescription,
) -> None:
    lone = member(u575, "tx_irq", "a", slice_wall_budget=60)
    want = asyncio.run(read_for(lone))
    assert want[0] == b"hello"
    for slice_instructions, third in VARIANTS:
        a = member(u575, "tx_irq", "a", slice_instructions)
        b = member(u575, "idle", "b", slice_instructions)
        links = [UartLink("a.USART1", "b.USART1", latency=L, baud=460_800)]
        if third is not None:
            links.append(UartLink("a.USART2", "b.USART2", latency=third, baud=460_800))
        with System({"a": a, "b": b}, links, slice_instructions=slice_instructions):
            assert asyncio.run(read_for(a)) == (want[0], StopReason.TIME)


def test_a_system_time_stop_gives_each_member_stop_its_reset_count(ping: Build) -> None:
    system = ping()
    before = asyncio.run(system.run(for_=us(100)))
    system.reset("a", popcorn.ResetKind.SOFTWARE)
    after = asyncio.run(system.run(for_=us(100)))
    assert before.reason is after.reason is StopReason.TIME
    assert before.stops["a"].resets >= 1
    assert after.stops["a"].resets == before.stops["a"].resets + 1
    assert after.stops["b"].resets == before.stops["b"].resets >= 1


async def stopped_in_running(
    m: popcorn.Machine, peer: popcorn.Machine | None = None
) -> list[tuple[object, ...]]:
    """Two waits inside `m.running()` that a breakpoint ends, then `peer` runs on, and the stop
    leaving raises."""
    never = m.ref.var("tx_next").ge(1_000_000)
    waited: list[Stop] = []
    with pytest.raises(popcorn.EmulationError, match="USER") as raised:
        async with m.running():
            waited.append(await m.wait_for(never))
            waited.append(await m.wait_for(never))
            if peer is not None:
                await asyncio.wait_for(peer.elapse(us(100)), 10)
    stops = [*waited, raised.value.stop]
    return [(s.reason, s.pc, s.instructions, s.time) for s in stops if s is not None]


def test_a_breakpoint_during_a_wait_ends_the_member_s_run_as_a_lone_machine_s(
    u575: popcorn.PartDescription,
) -> None:
    lone = asyncio.run(stopped_in_running(member(u575, "tx_irq", "a")))
    assert len(lone) == 3 and lone[0] == lone[1] == lone[2]
    assert lone[0][0] is StopReason.USER
    for slice_instructions, third in VARIANTS:
        a = member(u575, "tx_irq", "a", slice_instructions)
        b = member(u575, "idle", "b", slice_instructions)
        links = [UartLink("a.USART1", "b.USART1", latency=L, baud=460_800)]
        if third is not None:
            links.append(UartLink("a.USART2", "b.USART2", latency=third, baud=460_800))
        with System({"a": a, "b": b}, links, slice_instructions=slice_instructions) as system:
            assert asyncio.run(stopped_in_running(a, b)) == lone
            assert system._opens == {}  # pyright: ignore[reportPrivateUsage]
            assert system._handle.undecided() == []  # pyright: ignore[reportPrivateUsage]


def counting(m: popcorn.Machine) -> popcorn.Condition:
    """Count `m`'s USART1 interrupts in `quiet_bytes`, stopping right after the third count, with
    no event between the count and the stop. The condition the count reaches there."""

    def count(call: popcorn.Call) -> popcorn.Return:
        counted = m.var("quiet_bytes")
        counted.value = int(counted.value) + 1
        return call.stop() if int(counted.value) == 3 else call.proceed()

    m.intercept(IRQ, count)
    return m.ref.var("quiet_bytes").ge(3)


async def counted_in_running(m: popcorn.Machine, peer: popcorn.Machine) -> list[tuple[object, ...]]:
    """A wait on `counting`'s condition inside `m.running()`, then `peer` runs on, and the stop
    leaving raises."""
    reached = m.ref.var("quiet_bytes").ge(3)
    waited: list[Stop] = []
    with pytest.raises(popcorn.EmulationError, match="USER") as raised:
        async with m.running():
            waited.append(await m.wait_for(reached))
            await asyncio.wait_for(peer.elapse(us(100)), 10)
    stops = [*waited, raised.value.stop]
    return [point(s) for s in stops if s is not None]


def test_a_breakpoint_right_after_a_wait_s_condition_turns_true_ends_the_run(
    u575: popcorn.PartDescription,
) -> None:
    lone = member(u575, "tx_irq", "a", slice_wall_budget=60)
    reached = counting(lone)
    stop = asyncio.run(lone.wait_for(reached))
    assert stop.reason is StopReason.USER and stop.fired == ()
    assert reached.ready(lone)
    want = [point(stop), point(stop)]
    for slice_instructions, third in VARIANTS:
        a = member(u575, "tx_irq", "a", slice_instructions)
        b = member(u575, "idle", "b", slice_instructions)
        counting(a)
        links = [UartLink("a.USART1", "b.USART1", latency=L, baud=460_800)]
        if third is not None:
            links.append(UartLink("a.USART2", "b.USART2", latency=third, baud=460_800))
        with System({"a": a, "b": b}, links, slice_instructions=slice_instructions):
            assert asyncio.run(counted_in_running(a, b)) == want


async def both_waits(m: popcorn.Machine, cancel: str | None) -> tuple[object, ...]:
    """A Condition wait and a 4-byte read of `m`'s USART1 started together inside its
    `running()`, the wait never true and cancelled `cancel` when given."""
    asyncio.get_running_loop().set_task_factory(asyncio.eager_task_factory)
    target = 1_000 if cancel else 3
    got: list[object] = []
    with contextlib.suppress(popcorn.EmulationError):
        async with m.running():
            waiting = asyncio.create_task(m.wait_for(m.ref.var("tx_next").ge(target)))
            reading = asyncio.create_task(m.periph.USART1.port.read(4))
            if cancel == "later":
                for _ in range(5):
                    await asyncio.sleep(0)
            if cancel:
                waiting.cancel()
                with pytest.raises(asyncio.CancelledError):
                    await waiting
                got.append(None)
            else:
                got.append(point(await waiting))
            got.append(await reading)
    return tuple(got)


@pytest.mark.parametrize("cancel", [None, "at_once", "later"])
def test_two_waits_on_a_member_each_resolve_at_their_own_stop(
    u575: popcorn.PartDescription, cancel: str | None
) -> None:
    lone = member(u575, "tx_irq", "a", slice_wall_budget=60)
    stop = asyncio.run(lone.wait_for(lone.ref.var("tx_next").ge(3)))
    data = asyncio.run(member(u575, "tx_irq", "a").periph.USART1.port.read(4))
    want = (None if cancel else point(stop), data)
    assert data == b"hell"
    for slice_instructions, third in VARIANTS:
        a = member(u575, "tx_irq", "a", slice_instructions)
        b = member(u575, "idle", "b", slice_instructions)
        links = [UartLink("a.USART1", "b.USART1", latency=L, baud=460_800)]
        if third is not None:
            links.append(UartLink("a.USART2", "b.USART2", latency=third, baud=460_800))
        with System({"a": a, "b": b}, links, slice_instructions=slice_instructions) as system:
            assert asyncio.run(both_waits(a, cancel)) == want
            assert system._waits.get(0, []) == []  # pyright: ignore[reportPrivateUsage]


def tx_pair(
    u575: popcorn.PartDescription, slice_instructions: int, third: VirtualDuration | None
) -> tuple[popcorn.Machine, System]:
    a = member(u575, "tx_irq", "a", slice_instructions)
    b = member(u575, "idle", "b", slice_instructions)
    links = [UartLink("a.USART1", "b.USART1", latency=L, baud=460_800)]
    if third is not None:
        links.append(UartLink("a.USART2", "b.USART2", latency=third, baud=460_800))
    return a, System({"a": a, "b": b}, links, slice_instructions=slice_instructions)


async def reached_beside_elapse(m: popcorn.Machine, after: VirtualDuration) -> tuple[object, ...]:
    """A `Reached(IRQ)` wait and an `elapse(after)` started together inside `m.running()`."""
    asyncio.get_running_loop().set_task_factory(asyncio.eager_task_factory)
    async with m.running():
        reaching = asyncio.create_task(m.wait_for(popcorn.Reached(IRQ)))
        elapsed = await m.elapse(after)
        reached = await asyncio.wait_for(reaching, 10)
    return elapsed, point(reached)


def test_a_reached_wait_stands_at_its_address_when_another_wait_stops_there(
    u575: popcorn.PartDescription,
) -> None:
    lone = member(u575, "tx_irq", "a")
    start = lone.time
    stop = asyncio.run(lone.run(until=IRQ))
    assert stop.reason is StopReason.ADDRESS and stop.instructions > 0
    want = (stop.time, point(stop))
    for slice_instructions, third in VARIANTS:
        a, system = tx_pair(u575, slice_instructions, third)
        with system:
            assert asyncio.run(reached_beside_elapse(a, stop.time - start)) == want


async def reached_two(m: popcorn.Machine) -> tuple[object, ...]:
    """`Reached` waits for two addresses started together inside `m.running()`, the later
    address first."""
    asyncio.get_running_loop().set_task_factory(asyncio.eager_task_factory)
    async with m.running():
        late = asyncio.create_task(m.wait_for(popcorn.Reached(IRQ)))
        early = asyncio.create_task(m.wait_for(popcorn.Reached("enable")))
        first = await asyncio.wait_for(early, 10)
        second = await asyncio.wait_for(late, 10)
    return point(first), point(second)


def test_reached_waits_for_two_addresses_each_stop_at_their_own(
    u575: popcorn.PartDescription,
) -> None:
    early = asyncio.run(member(u575, "tx_irq", "a").run(until="enable"))
    late = asyncio.run(member(u575, "tx_irq", "a").run(until=IRQ))
    assert early.reason is late.reason is StopReason.ADDRESS
    assert early.instructions < late.instructions
    want = (point(early), point(late))
    for slice_instructions, third in VARIANTS:
        a, system = tx_pair(u575, slice_instructions, third)
        with system:
            assert asyncio.run(reached_two(a)) == want


def test_a_run_inside_a_member_s_running_raises_machine_busy(
    u575: popcorn.PartDescription,
) -> None:
    a, system = tx_pair(u575, 1_000_000, None)

    async def body() -> None:
        with contextlib.suppress(popcorn.EmulationError):
            async with a.running():
                with pytest.raises(popcorn.MachineBusy, match="no run inside it"):
                    await a.run(until=IRQ, budget=10)
                with pytest.raises(popcorn.MachineBusy, match="no run inside it"):
                    await a.run(until=IRQ)

    with system:
        asyncio.run(body())
        assert system._waits.get(0, []) == []  # pyright: ignore[reportPrivateUsage]


@pytest.mark.parametrize("point", ["before", "settle", "wait"])
def test_a_cancelled_wait_inside_running_lets_the_member_run_on(
    u575: popcorn.PartDescription, point: str
) -> None:
    a = member(u575, "idle", "a")
    b = member(u575, "tx_irq", "b")
    gate = threading.Event()
    called = threading.Event()

    def held(_: popcorn.Machine) -> bool:
        called.set()
        gate.wait(10)
        return False

    system = System({"a": a, "b": b}, [UartLink("a.USART1", "b.USART1", latency=L, baud=460_800)])

    async def body() -> VirtualTime:
        async with a.running():
            opens = system._opens  # pyright: ignore[reportPrivateUsage]
            first = opens[0]
            peer = None
            if point == "settle":
                peer = asyncio.create_task(b.run(stop_when=held, for_=ms(1)))
                while not called.is_set():
                    await asyncio.sleep(0.001)
            waiting = asyncio.create_task(a.wait_for(a.ref.var("tx_next").ge(100)))
            if point == "settle":
                while not first.withdrawn:
                    await asyncio.sleep(0)
            elif point == "wait":
                while opens[0] is first:
                    await asyncio.sleep(0)
            waiting.cancel()
            gate.set()
            with pytest.raises(asyncio.CancelledError):
                await waiting
            while opens[0].withdrawn or not opens[0].releases:
                await asyncio.sleep(0.001)
            elapsed = await asyncio.wait_for(a.elapse(us(100)), 10)
            if peer is not None:
                await asyncio.wait_for(peer, 10)
            return elapsed

    with system:
        before = a.time
        ended = asyncio.run(body())
        assert ended.ps >= before.ps + us(100).ps
        assert system._opens == {}  # pyright: ignore[reportPrivateUsage]
        assert system._goals == {}  # pyright: ignore[reportPrivateUsage]


def test_a_wait_cancelled_as_its_system_closes_reports_nothing(
    u575: popcorn.PartDescription, caplog: pytest.LogCaptureFixture
) -> None:
    a = member(u575, "idle", "a")
    b = member(u575, "tx_irq", "b")
    system = System({"a": a, "b": b}, [UartLink("a.USART1", "b.USART1", latency=L, baud=460_800)])

    async def body() -> None:
        with contextlib.suppress(popcorn.EmulationError):
            async with a.running():
                opens = system._opens  # pyright: ignore[reportPrivateUsage]
                first = opens[0]
                waiting = asyncio.create_task(a.wait_for(a.ref.var("tx_next").ge(100)))
                while opens[0] is first:
                    await asyncio.sleep(0)
                waiting.cancel()
                with pytest.raises(asyncio.CancelledError):
                    await waiting
                system.close()
                for _ in range(10):
                    await asyncio.sleep(0.001)

    with caplog.at_level(logging.ERROR, logger="asyncio"):
        asyncio.run(body())
        gc.collect()
    assert [r.getMessage() for r in caplog.records if r.name == "asyncio"] == []


def flying(system: System) -> bool:
    """Whether `system` has a resume in flight."""
    return system._flight is not None  # pyright: ignore[reportPrivateUsage]


def test_a_flight_cut_by_a_closed_system_and_a_loop_s_end_reports_nothing(
    u575: popcorn.PartDescription, caplog: pytest.LogCaptureFixture
) -> None:
    with caplog.at_level(logging.ERROR, logger="asyncio"):
        refs = cut_flight(u575)
    assert [r.getMessage() for r in caplog.records if r.name == "asyncio"] == []
    for _ in range(1000):
        gc.collect()
        if all(one() is None for one in refs):
            break
        time.sleep(0.01)
    assert [one() for one in refs] == [None, None]


def cut_flight(u575: popcorn.PartDescription) -> list[weakref.ref[popcorn.Machine]]:
    """A flight held by a blocked `stop_when` when the system closes and the loop ends: weakrefs
    to the members, their thread then let go."""
    a = member(u575, "idle", "a")
    b = member(u575, "tx_irq", "b")
    gate = threading.Event()
    called = threading.Event()

    def held(_: popcorn.Machine) -> bool:
        called.set()
        gate.wait(10)
        return False

    system = System({"a": a, "b": b}, [UartLink("a.USART1", "b.USART1", latency=L, baud=460_800)])

    async def body() -> None:
        goal = asyncio.create_task(b.run(stop_when=held, for_=ms(1)))
        while not called.is_set():
            await asyncio.sleep(0.001)
        threading.Timer(0.2, gate.set).start()
        system.close()
        goal.cancel()

    asyncio.run(body())
    gate.set()
    return [weakref.ref(a), weakref.ref(b)]


OFFSETS = {"a": ms(1), "b": us(500)}


Converted = tuple[list[VirtualTime], list[VirtualTime], tuple[object, ...], tuple[object, ...]]


def test_member_times_convert_both_ways_at_any_quantum(ping: Build) -> None:
    async def body(system: System) -> Converted:
        a, b = system.members["a"], system.members["b"]
        bases = [system.to_system(name, VirtualTime(0)) for name in ("a", "b")]
        assert [base.ps for base in bases] == [ms(1).ps - a.time.ps, us(500).ps - b.time.ps]
        a_stop, b_stop = await asyncio.gather(a.run(until=IRQ), b.run(for_=ECHOED))
        for name, m in (("a", a), ("b", b)):
            assert system.to_system(name, m.time) == system.time_of(name)
            assert system.to_system(m, m.time) == system.time_of(name)
            assert system.to_local(name, system.time_of(name)) == m.time
        running = asyncio.create_task(a.run(for_=ms(1)))
        while system._flight is None:  # pyright: ignore[reportPrivateUsage]
            await asyncio.sleep(0)
        during = [system.to_system(name, VirtualTime(0)) for name in ("a", "b")]
        await running
        with pytest.raises(KeyError):
            system.to_local("c", VirtualTime(0))
        with pytest.raises(TypeError):
            system.to_local("a", 5)  # pyright: ignore[reportArgumentType]
        with pytest.raises(ValueError, match="before the last reset of a"):
            system.to_local("a", VirtualTime(bases[0].ps - 1))
        return bases, during, point(a_stop), point(b_stop)

    results = [
        asyncio.run(body(ping(slice_instructions, third, offsets=OFFSETS)))
        for slice_instructions, third in VARIANTS
    ]
    assert results[1:] == results[:1] * 3
    bases, during, _, _ = results[0]
    assert during == bases


async def close_in_flight(system: System) -> None:
    """Close `system` during a long goal of b, with a held: b's first goal leaves a halted with
    its core held, and only `finish()` would give it back."""
    b = system.members["b"]
    await b.run(for_=us(100))
    goal = asyncio.create_task(b.run(for_=s(100)))
    while system._flight is None:  # pyright: ignore[reportPrivateUsage]
        await asyncio.sleep(0)
    system.close()
    with contextlib.suppress(Exception):
        await goal


@pytest.mark.parametrize(("slice_instructions", "third"), VARIANTS)
def test_a_member_halted_when_its_system_closes_runs_alone_at_once(
    ping: Build, slice_instructions: int, third: VirtualDuration | None
) -> None:
    async def body(system: System) -> tuple[Stop, Stop]:
        a = system.members["a"]
        await close_in_flight(system)
        return await a.run(budget=10), await a.run(for_=us(10))

    by_budget, by_time = asyncio.run(body(ping(slice_instructions, third)))
    assert by_budget.reason is StopReason.BUDGET
    assert by_time.reason is StopReason.TIME


@pytest.mark.parametrize("direct", [False, True])
@pytest.mark.parametrize(("slice_instructions", "third"), VARIANTS)
def test_a_member_halted_when_its_system_closes_takes_a_call_at_once(
    ping: Build, slice_instructions: int, third: VirtualDuration | None, direct: bool
) -> None:
    async def body(system: System) -> Stop | None:
        a = system.members["a"]
        await close_in_flight(system)
        if not direct:
            return (await a.call("link_ping", budget=1000)).stop
        frame = _core.CallFrame()
        frame.entry = a.address_of("link_ping")
        frame.return_sentinel = a._sentinel  # pyright: ignore[reportPrivateUsage]
        a._handle.begin_call(frame)  # pyright: ignore[reportPrivateUsage]
        assert a._handle.in_call()  # pyright: ignore[reportPrivateUsage]
        a._handle.end_call()  # pyright: ignore[reportPrivateUsage]
        return None

    stop = asyncio.run(body(ping(slice_instructions, third)))
    assert stop is None or stop.reason is StopReason.BUDGET


def test_member_times_convert_from_a_member_s_callback_as_after_the_run(ping: Build) -> None:
    async def body(system: System) -> list[tuple[VirtualTime, ...]]:
        a, b = system.members["a"], system.members["b"]
        seen: list[tuple[VirtualTime, ...]] = []

        def convert(call: popcorn.Call) -> popcorn.Return:
            local = b.time
            at = system.to_system("b", local)
            seen.append(
                (
                    local,
                    at,
                    system.to_system(b, local),
                    system.to_local("b", at),
                    system.to_local(a, at),
                )
            )
            return call.proceed()

        b.intercept(IRQ, convert)
        await asyncio.gather(a.run(for_=ECHOED), b.run(for_=ECHOED))
        assert len(seen) >= 2
        for local, at, by_machine, back, a_local in seen:
            assert at == by_machine == system.to_system("b", local)
            assert back == local == system.to_local("b", at)
            assert a_local == system.to_local("a", at)
        return seen

    across(ping, body)


def test_a_stamped_write_on_a_halted_member_lands_at_its_system_time(ping: Build) -> None:
    target = VirtualTime(ms(1.5).ps)

    async def body(system: System) -> object:
        a, b = system.members["a"], system.members["b"]
        address = b.sym("received_count")
        value = b.mem.u32[address]
        a_stop, written = await asyncio.gather(
            a.run(for_=ms(2)),
            b.write((b.ref.mem.u32[address], value), at=system.to_local("b", target)),
        )
        assert system.to_system("b", written.time) == target
        return point(a_stop), written.time

    across(ping, body)


def test_a_member_reset_moves_its_base_to_the_reset_s_system_time(ping: Build) -> None:
    async def body(system: System) -> object:
        a, b = system.members["a"], system.members["b"]
        await b.run(for_=ms(1))
        b.halt()
        at = system.time_of("b")
        system._handle.reset(1, _core.ResetKind.power_on)  # pyright: ignore[reportPrivateUsage]
        assert b.time == VirtualTime(0)
        assert system.to_system("b", VirtualTime(0)) == at
        assert system.to_local("b", at) == VirtualTime(0)
        stop = await a.run(for_=ms(1))
        assert system.to_system("b", b.time) == system.time_of("b")
        return at, point(stop), system.time_of("b")

    across(ping, body)
