"""Each member's `last_stop` after a `System.run` and `System.wait_for`."""

from __future__ import annotations

import asyncio
from collections.abc import Iterator

import popcorn
import pytest
from popcorn import StopReason, System, UartLink
from popcorn.units import VirtualTime, ms, s, us

from .linked import member

pytestmark = pytest.mark.usefixtures("both_sync_modes")


@pytest.fixture
def ping(u575: popcorn.PartDescription) -> Iterator[System]:
    a = member(u575, "link_ping", "a")
    b = member(u575, "link_echo", "b")
    with System(
        {"a": a, "b": b}, [UartLink("a.USART1", "b.USART1", latency=us(20), baud=460_800)]
    ) as system:
        yield system


def settled(system: System) -> popcorn.SystemStop:
    first = asyncio.run(system.run(for_=ms(3)))
    assert (first.reason, first.member) == (StopReason.USER, "a")
    done = asyncio.run(system.run(until=VirtualTime(ms(3).ps)))
    assert done.reason is StopReason.TIME
    return done


def test_a_system_run_replaces_each_member_stop(ping: System) -> None:
    a, b = ping.members["a"], ping.members["b"]
    first = asyncio.run(ping.run(for_=ms(3)))
    assert (first.reason, first.member) == (StopReason.USER, "a")
    assert a.last_stop == first.stop
    assert b.last_stop is None
    asyncio.run(ping.run(until=VirtualTime(ms(3).ps)))
    own = asyncio.run(a.run(budget=1_000))
    assert a.last_stop == own
    a.halt()
    done = asyncio.run(ping.run(for_=ms(1)))
    assert done.reason is StopReason.TIME
    for name, machine in ping.members.items():
        assert machine.last_stop == done.stops[name]
        assert machine.last_stop is not None
        assert machine.last_stop.reason is StopReason.TIME
    assert a.last_stop != own
    assert b.where() == done.stops["b"].location


def test_a_cancelled_system_run_leaves_no_member_stop(ping: System) -> None:
    settled(ping)
    assert all(m.last_stop is not None for m in ping.members.values())

    async def cancelled() -> None:
        with pytest.raises(TimeoutError):
            await asyncio.wait_for(ping.run(for_=s(100)), 0.05)

    asyncio.run(cancelled())
    assert all(m.last_stop is None for m in ping.members.values())


def test_wait_for_leaves_the_check_point_stops_also_when_the_predicate_raises(
    ping: System,
) -> None:
    settled(ping)
    held = asyncio.run(ping.wait_for(lambda _: True, every=us(100)))
    assert held.reason is StopReason.USER
    for name, machine in ping.members.items():
        assert machine.last_stop == held.stops[name]
        assert machine.last_stop is not None
        assert machine.last_stop.reason is StopReason.TIME
    seen: dict[str, popcorn.Stop | None] = {}

    def broken(system: System) -> bool:
        seen.update((name, m.last_stop) for name, m in system.members.items())
        raise LookupError("predicate")

    with pytest.raises(LookupError):
        asyncio.run(ping.wait_for(broken, every=us(100)))
    for name, machine in ping.members.items():
        stop = machine.last_stop
        assert stop is not None and stop == seen[name] and stop != held.stops[name]
        assert stop.reason is StopReason.TIME


def test_a_free_run_clears_every_member_stop(ping: System) -> None:
    settled(ping)
    assert all(m.last_stop is not None for m in ping.members.values())
    for machine in ping.members.values():
        machine.reg.pc = machine.sym("idle")

    async def free() -> None:
        async with ping.running():
            await asyncio.sleep(0)

    asyncio.run(free())
    assert all(m.last_stop is None for m in ping.members.values())


def test_a_member_stop_of_the_free_run_is_its_last_stop(ping: System) -> None:
    settled(ping)
    a, b = ping.members["a"], ping.members["b"]

    async def free() -> popcorn.EmulationError:
        ping.start()
        opens = ping._opens  # pyright: ignore[reportPrivateUsage]
        while not (opens.get(0) is not None and opens[0].done()):
            await asyncio.sleep(0)
        with pytest.raises(popcorn.EmulationError, match="USER") as raised:
            ping.halt()
        return raised.value

    error = asyncio.run(free())
    assert error.stop is not None
    assert a.last_stop == error.stop
    assert b.last_stop is None
