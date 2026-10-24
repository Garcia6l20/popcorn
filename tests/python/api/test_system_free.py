"""A `System`'s free run: `start()`, `halt()` and `running()`, every member's core running
until halted, member waits on it, and the system's `wait_for` at fixed check points."""

from __future__ import annotations

import asyncio
import gc
import threading
import weakref
from collections.abc import Callable, Coroutine, Iterator
from typing import Any

import popcorn
import pytest
from popcorn import Stop, StopReason, System, UartLink
from popcorn._errors import MachineBusy
from popcorn.units import VirtualDuration, VirtualTime, ms, us

from .linked import member

pytestmark = pytest.mark.usefixtures("both_sync_modes")


L = us(20)
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

    def build(slice_instructions: int = 1_000_000, third: VirtualDuration | None = None) -> System:
        a = member(u575, "link_ping", "a", slice_instructions)
        b = member(u575, "link_echo", "b", slice_instructions)
        links = [UartLink("a.USART1", "b.USART1", latency=L, baud=460_800)]
        if third is not None:
            links.append(UartLink("a.USART2", "b.USART2", latency=third, baud=460_800))
        system = System({"a": a, "b": b}, links, slice_instructions=slice_instructions)
        made.append(system)
        return system

    yield build
    for one in made:
        one.close()


BKPT_DONE = 0xBE01


def at_done(m: popcorn.Machine, stop: Stop | None) -> bool:
    """Whether `stop` is link_ping's DONE on `m`: USER right after its `bkpt #1`."""
    return (
        stop is not None
        and stop.reason is StopReason.USER
        and stop.location.symbol == "link_ping"
        and m.mem.u16[stop.pc - 2] == BKPT_DONE
    )


def bounded(main: Coroutine[Any, Any, None], timeout: float = 30) -> None:
    """`asyncio.run(main)` in a daemon thread, failing once it runs past `timeout` seconds, so a
    hang fails the test instead of stalling the session."""
    failed: list[BaseException] = []

    def run() -> None:
        try:
            asyncio.run(main)
        except BaseException as error:
            failed.append(error)

    runner = threading.Thread(target=run, daemon=True)
    runner.start()
    runner.join(timeout)
    assert not runner.is_alive(), f"still running after {timeout} s"
    if failed:
        raise failed[0]


def across(ping: Build, body: Callable[[System], Coroutine[Any, Any, object]]) -> object:
    """`body`'s result on a fresh system in every variant, checked equal, and returned."""
    results: list[object] = []
    for slice_instructions, third in VARIANTS:
        system = ping(slice_instructions, third)
        results.append(asyncio.run(body(system)))
    assert results[1:] == results[:1] * (len(results) - 1)
    return results[0]


def point(stop: Stop) -> tuple[object, ...]:
    return (stop.reason, stop.pc, stop.instructions, stop.time, [str(one) for one in stop.fired])


def clean(system: System) -> bool:
    """Whether `system` keeps nothing of a run: no flight, goal, open goal, end or parked member."""
    return (
        system._flight is None  # pyright: ignore[reportPrivateUsage]
        and system._goals == {}  # pyright: ignore[reportPrivateUsage]
        and system._opens == {}  # pyright: ignore[reportPrivateUsage]
        and system._ends == []  # pyright: ignore[reportPrivateUsage]
        and not any(system._waits.values())  # pyright: ignore[reportPrivateUsage]
        and system._undecided == {}  # pyright: ignore[reportPrivateUsage]
        and system._handle.undecided() == []  # pyright: ignore[reportPrivateUsage]
        and not system._free  # pyright: ignore[reportPrivateUsage]
    )


def test_a_wait_in_the_system_s_free_run_stops_exactly_at_any_slicing(ping: Build) -> None:
    async def body(system: System) -> object:
        a = system.members["a"]
        echoed = a.ref.var("log_len").ge(10)
        async with system.running():
            stop = await a.wait_for(echoed)
            assert system._free  # pyright: ignore[reportPrivateUsage]
        assert clean(system)
        return point(stop)

    stop = across(ping, body)
    assert isinstance(stop, tuple) and stop[0] is StopReason.USER


def test_a_member_stop_ends_its_run_only_and_leaving_raises_it(ping: Build) -> None:
    async def body(system: System) -> object:
        a, b = system.members["a"], system.members["b"]
        waited: list[Stop] = []
        with pytest.raises(popcorn.EmulationError, match="USER") as raised:
            async with system.running():
                waited.append(await a.wait_for(a.ref.var("log_len").ge(1_000_000)))
                waited.append(await a.wait_for(a.ref.var("log_len").ge(1_000_000)))
                before = (await b.read()).time
                after = await b.elapse(us(100))
                assert after.ps >= before.ps + us(100).ps
        assert waited[0] == waited[1] == raised.value.stop
        assert clean(system)
        return point(waited[0])

    stop = across(ping, body)
    assert isinstance(stop, tuple) and stop[0] is StopReason.USER


def test_a_member_stop_with_no_wait_is_raised_on_leaving(ping: Build) -> None:
    async def body(system: System) -> object:
        b = system.members["b"]
        with pytest.raises(popcorn.EmulationError, match="USER") as raised:
            async with system.running():
                await b.elapse(ms(3))
        assert str(raised.value).startswith("a: ")
        assert clean(system)
        stop = raised.value.stop
        assert stop is not None
        return point(stop)

    across(ping, body)


def test_a_sync_halt_parks_every_member_and_a_run_follows(ping: Build) -> None:
    system = ping()
    a, b = system.members["a"], system.members["b"]

    async def body() -> None:
        system.start()
        assert system._free  # pyright: ignore[reportPrivateUsage]
        await b.elapse(us(200))
        system.halt()
        assert clean(system)
        assert a.pc and b.pc
        system.halt()

    asyncio.run(body())
    stop = asyncio.run(system.run(for_=ms(1)))
    assert stop.reason is StopReason.TIME


def test_a_wait_pending_when_the_free_run_halts_raises(ping: Build) -> None:
    system = ping()
    b = system.members["b"]

    async def body() -> None:
        system.start()
        opens = system._opens  # pyright: ignore[reportPrivateUsage]
        first = opens[1]
        waiting = asyncio.create_task(b.wait_for(b.ref.var("received_count").eq(7)))
        while opens.get(1) is first:
            await asyncio.sleep(0)
        system.halt()
        with pytest.raises(popcorn.EmulationError):
            await waiting

    asyncio.run(body())
    assert clean(system)


def test_a_halt_releases_a_member_its_delivered_wait_left_parked(ping: Build) -> None:
    system = ping()
    b = system.members["b"]

    async def body() -> None:
        system.start()
        opens = system._opens  # pyright: ignore[reportPrivateUsage]
        first = opens[1]
        waiting = asyncio.create_task(b.elapse(us(30)))
        while opens.get(1) is first or not opens[1].done():
            await asyncio.sleep(0)
        assert system._handle.undecided() == [1]  # pyright: ignore[reportPrivateUsage]
        system.halt()
        assert clean(system)
        await waiting
        parked = b.executed
        await system.run(for_=ms(1))
        assert b.executed > parked

    asyncio.run(body())
    assert clean(system)


def test_free_run_refusals(ping: Build) -> None:
    system = ping()
    a = system.members["a"]
    with pytest.raises(RuntimeError):
        system.start()
    system.halt()

    async def body() -> None:
        async with system.running():
            with pytest.raises(MachineBusy):
                system.start()
            with pytest.raises(MachineBusy):
                await system.run(for_=ms(1))
            with pytest.raises(MachineBusy):
                async with a.running():
                    pass
            with pytest.raises(MachineBusy):
                a.halt()
            await a.elapse(us(10))
        goal = asyncio.create_task(a.run(until="idle", budget=10**12))
        await asyncio.sleep(0)
        with pytest.raises(MachineBusy):
            system.start()
        goal.cancel()
        with pytest.raises(asyncio.CancelledError):
            await goal
        async with a.running():
            with pytest.raises(MachineBusy):
                system.start()
            await a.elapse(us(10))

    asyncio.run(body())
    assert clean(system)


def test_a_free_run_with_native_waits_takes_no_gil(ping: Build) -> None:
    system = ping()
    a, b = system.members["a"], system.members["b"]
    before = [m._handle.gil_entries() for m in (a, b)]  # pyright: ignore[reportPrivateUsage]

    async def body() -> None:
        async with system.running():
            await a.wait_for(popcorn.Reached("USART1_IRQHandler"))
            await b.elapse(us(50))

    asyncio.run(body())
    gc.collect()
    after = [m._handle.gil_entries() for m in (a, b)]  # pyright: ignore[reportPrivateUsage]
    assert after == before


def test_many_free_runs_leave_no_entry(ping: Build) -> None:
    system = ping()
    b = system.members["b"]

    async def body() -> None:
        for _ in range(200):
            async with system.running():
                await b.elapse(us(1))

    asyncio.run(body())
    assert clean(system)
    assert system._run_ends == {}  # pyright: ignore[reportPrivateUsage]
    assert system._rebuilds == set()  # pyright: ignore[reportPrivateUsage]
    assert system._withdrawals == set()  # pyright: ignore[reportPrivateUsage]


class Held:
    pass


def free_path(u575: popcorn.PartDescription, path: str) -> list[weakref.ref[object]]:
    """A system free run ending by `path`, then the system closed: weakrefs to what it held."""
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
                async with system.running():
                    while system._failed is None:  # pyright: ignore[reportPrivateUsage]
                        await asyncio.sleep(0.001)
                    assert kept is held
        elif path == "closed":
            async with system.running():
                system.close()
                assert kept is held
        elif path == "raised":
            with pytest.raises(ValueError):
                async with system.running():
                    raise ValueError(kept)
        elif path == "stopped":
            with pytest.raises(popcorn.EmulationError, match="USER"):
                async with system.running():
                    await a.wait_for(a.ref.var("log_len").ge(1_000_000))
                    assert kept is held
        else:
            async with system.running():
                await b.elapse(us(100))
                assert kept is held

    asyncio.run(body())
    system.close()
    return [weakref.ref(held), weakref.ref(a), weakref.ref(system)]


@pytest.mark.parametrize("path", ["plain", "raised", "stopped", "closed", "failed"])
def test_a_free_run_frees_what_it_held(u575: popcorn.PartDescription, path: str) -> None:
    refs = free_path(u575, path)
    gc.collect()
    assert [one() for one in refs] == [None, None, None]


def echoes(system: System) -> int:
    """Log words of member a, two per echo it received."""
    return int(system.members["a"].var("log_len").value)


def test_a_system_wait_stops_at_the_first_check_point_where_it_holds(ping: Build) -> None:
    every = us(50)

    async def body(system: System) -> object:
        seen: list[int] = []

        def echoed(s: System) -> bool:
            seen.append(echoes(s))
            return seen[-1] >= 8

        stop = await system.wait_for(echoed, every=every)
        assert stop.reason is StopReason.USER and stop.member is None
        assert stop.time == VirtualTime(len(seen) * every.ps)
        assert seen[-1] >= 8 and all(one < 8 for one in seen[:-1])
        later = await system.run(for_=us(10))
        assert later.time == VirtualTime(stop.time.ps + us(10).ps)
        return stop.time, seen, {name: point(one) for name, one in stop.stops.items()}

    across(ping, body)


def test_a_system_wait_ends_at_for_without_asking_there(ping: Build) -> None:
    system = ping()
    asked: list[VirtualTime] = []

    def never(s: System) -> bool:
        asked.append(s.time_of("b"))
        return False

    stop = asyncio.run(system.wait_for(never, every=us(300), for_=us(900)))
    assert stop.reason is StopReason.TIME
    assert stop.time == VirtualTime(us(900).ps)
    assert asked == [VirtualTime(us(300).ps), VirtualTime(us(600).ps)]
    asked.clear()
    stop = asyncio.run(system.wait_for(never, every=us(40), for_=us(100)))
    assert stop.time == VirtualTime(us(1000).ps)
    assert asked == [VirtualTime(us(940).ps), VirtualTime(us(980).ps)]


def test_a_member_stop_ends_a_system_wait(ping: Build) -> None:
    async def body(system: System) -> object:
        stop = await system.wait_for(lambda _: False, every=us(100))
        assert stop.member == "a" and stop.reason is StopReason.USER
        assert stop.stop is not None
        return point(stop.stop), stop.time

    across(ping, body)


def test_system_wait_refusals(ping: Build) -> None:
    system = ping()

    def nested(s: System) -> bool:
        with pytest.raises(MachineBusy):
            s.start()
        return True

    def raising(_: System) -> bool:
        raise KeyError("asked")

    async def body() -> None:
        with pytest.raises(TypeError):
            await system.wait_for(lambda _: True, every=50)  # pyright: ignore[reportArgumentType]
        with pytest.raises(ValueError):
            await system.wait_for(lambda _: True, every=us(0))
        with pytest.raises(ValueError):
            await system.wait_for(lambda _: True, every=us(1), for_=VirtualDuration(-1))
        with pytest.raises(KeyError):
            await system.wait_for(raising, every=us(10))
        assert (await system.wait_for(nested, every=us(10))).time == VirtualTime(us(20).ps)
        assert (await system.run(for_=us(10))).time == VirtualTime(us(30).ps)
        async with system.running():
            with pytest.raises(MachineBusy):
                await system.wait_for(lambda _: True, every=us(10))

    asyncio.run(body())


def test_a_halt_from_another_thread_runs_on_the_free_run_s_loop(ping: Build) -> None:
    system = ping()
    b = system.members["b"]
    outcome: list[BaseException | None] = []

    def halt() -> None:
        try:
            system.halt()
        except BaseException as error:
            outcome.append(error)
        else:
            outcome.append(None)

    async def body() -> None:
        asyncio.get_running_loop().set_debug(True)
        system.start()
        await b.elapse(us(50))
        waiting = asyncio.create_task(b.wait_for(b.ref.var("received_count").eq(7)))
        await asyncio.sleep(0)
        halting = threading.Thread(target=halt, daemon=True)
        halting.start()
        while halting.is_alive():
            await asyncio.sleep(0.001)
        assert len(outcome) == 1
        raised = outcome[0]
        if raised is not None:
            assert isinstance(raised, popcorn.EmulationError)
            assert at_done(system.members["a"], raised.stop)
        with pytest.raises(popcorn.EmulationError):
            await asyncio.wait_for(waiting, 10)
        assert clean(system)

    bounded(body())
    after = asyncio.run(system.run(for_=ms(1)))
    if after.reason is StopReason.USER:
        assert after.member == "a"
        assert at_done(system.members["a"], after.stop)
    else:
        assert after.reason is StopReason.TIME


def test_a_halt_waiting_for_a_stopped_loop_ends_when_the_system_closes(ping: Build) -> None:
    system = ping()
    loop = asyncio.new_event_loop()
    outcome: list[BaseException | None] = []

    def halt() -> None:
        try:
            system.halt()
        except BaseException as error:
            outcome.append(error)
        else:
            outcome.append(None)

    async def start() -> None:
        system.start()

    halting = threading.Thread(target=halt, daemon=True)
    try:
        loop.run_until_complete(start())
        halting.start()
        halting.join(0.2)
        assert halting.is_alive()
        system.close()
        halting.join(10)
    finally:
        loop.run_until_complete(asyncio.sleep(0))
        loop.close()
    assert not halting.is_alive()
    assert len(outcome) == 1
    raised = outcome[0]
    if raised is not None:
        assert isinstance(raised, popcorn.EmulationError)
        assert at_done(system.members["a"], raised.stop)


def test_two_eager_running_blocks_each_end_their_free_run(ping: Build) -> None:
    system = ping()

    async def body() -> list[bool]:
        asyncio.get_running_loop().set_task_factory(asyncio.eager_task_factory)
        ended: list[bool] = []
        for _ in range(2):
            async with system.running():
                pass
            ended.append(clean(system))
        return ended

    assert asyncio.run(body()) == [True, True]
    after = asyncio.run(system.run(for_=ms(1)))
    if after.reason is StopReason.USER:
        assert after.member == "a"
        assert at_done(system.members["a"], after.stop)
    else:
        assert after.reason is StopReason.TIME


def test_a_halt_from_a_machine_callback_raises_runtime_error(ping: Build) -> None:
    system = ping()
    a = system.members["a"]
    outcome: list[BaseException | None] = []

    def halt_inside(call: popcorn.Call) -> popcorn.Return:
        try:
            system.halt()
        except BaseException as error:
            outcome.append(error)
        else:
            outcome.append(None)
        return call.proceed()

    async def body() -> None:
        a.intercept("send_byte", halt_inside, once=True)
        async with system.running():
            while not outcome:
                await asyncio.sleep(0.001)

    bounded(body())
    assert len(outcome) == 1
    assert isinstance(outcome[0], RuntimeError)
    assert "callback of a machine" in str(outcome[0])


def test_a_halt_from_a_machine_callback_with_no_free_run_keeps_what_it_would_raise(
    ping: Build,
) -> None:
    system = ping()
    a = system.members["a"]
    kept = ValueError("kept")
    system._free_error = kept  # pyright: ignore[reportPrivateUsage]
    outcome: list[BaseException | None] = []

    def halt_inside(call: popcorn.Call) -> popcorn.Return:
        try:
            system.halt()
        except BaseException as error:
            outcome.append(error)
        else:
            outcome.append(None)
        return call.proceed()

    async def body() -> None:
        a.intercept("send_byte", halt_inside, once=True)
        await system.run(for_=ms(3))

    asyncio.run(body())
    assert len(outcome) == 1
    assert isinstance(outcome[0], RuntimeError)
    assert "callback of a machine" in str(outcome[0])
    with pytest.raises(ValueError, match="kept") as raised:
        system.halt()
    assert raised.value is kept
    system.halt()


def test_a_forwarded_halt_falls_back_only_when_its_loop_closed(ping: Build) -> None:
    system = ping()
    closed = asyncio.new_event_loop()
    closed.close()
    forwarded: list[bool] = []

    async def body() -> None:
        forwarded.append(system._halt_from(closed))  # pyright: ignore[reportPrivateUsage]

    bounded(body())
    assert forwarded == [False]


def test_a_halt_from_another_thread_waits_for_a_stopped_loop_to_run_again(ping: Build) -> None:
    system = ping()
    loop = asyncio.new_event_loop()
    outcome: list[BaseException | None] = []

    def halt() -> None:
        try:
            system.halt()
        except BaseException as error:
            outcome.append(error)
        else:
            outcome.append(None)

    async def start() -> None:
        system.start()

    halting = threading.Thread(target=halt, daemon=True)
    try:
        loop.run_until_complete(start())
        halting.start()
        halting.join(0.2)
        assert halting.is_alive()
        loop.run_until_complete(asyncio.sleep(0.05))
        halting.join(10)
    finally:
        loop.close()
    assert not halting.is_alive()
    assert len(outcome) == 1
    raised = outcome[0]
    if raised is not None:
        assert isinstance(raised, popcorn.EmulationError)
        assert at_done(system.members["a"], raised.stop)
    assert clean(system)


def test_a_member_stop_ending_a_wait_just_before_halt_is_raised(ping: Build) -> None:
    async def body(system: System) -> None:
        a = system.members["a"]
        asyncio.get_running_loop().set_task_factory(asyncio.eager_task_factory)
        system.start()
        waiting = asyncio.create_task(a.wait_for(a.ref.var("log_len").ge(1_000_000)))
        opens = system._opens  # pyright: ignore[reportPrivateUsage]
        while not (opens.get(0) is not None and opens[0].waits and opens[0].done()):
            if waiting.done():
                pytest.fail("the member stopped before the goal serving the wait was posted")
            await asyncio.sleep(0)
        assert not waiting.done()
        with pytest.raises(popcorn.EmulationError, match="USER") as raised:
            system.halt()
        stop = await waiting
        assert raised.value.stop == stop
        assert clean(system)

    for _ in range(2):
        asyncio.run(body(ping()))


def test_a_probe_on_a_member_of_a_running_system_counts_but_is_changed_outside_it(
    ping: Build,
) -> None:
    system = ping()
    a = system.members["a"]
    irq = a.probe("USART1_IRQHandler")

    async def body() -> int:
        async with system.running():
            with pytest.raises(MachineBusy):
                await irq.hit(within=ms(1))
            with pytest.raises(popcorn.MachineRunning):
                a.probe("idle")
            with pytest.raises(popcorn.MachineRunning):
                irq.remove()
            assert irq.active
            await a.wait_until(lambda _: irq.count > 0, for_=ms(10))
            return irq.count

    counted = asyncio.run(body())
    assert counted > 0
    assert asyncio.run(irq.hit(within=ms(1)))
    irq.remove()
    assert not irq.active
    assert clean(system)
