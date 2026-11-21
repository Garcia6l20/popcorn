"""A `Condition` or `Conditions` wait on a member of a linked system: evaluated natively behind a
gate, `Stop.fired` captured at the stop instant."""

from __future__ import annotations

import asyncio
from collections.abc import Awaitable, Callable

import popcorn
import pytest
from popcorn import Condition, StopReason, System, _core, all_of, any_of
from popcorn._refs import Conditions
from popcorn.units import ms

from .linked import Build

LOG_WORDS_PER_ECHO = 2
Wait = Callable[[System, Condition | Conditions], Awaitable[popcorn.Stop]]


@pytest.fixture
def ping(pair: Build) -> Callable[[], System]:
    return lambda: pair(a_entry="link_ping", b_entry="link_echo")


@pytest.fixture
def ready_calls(monkeypatch: pytest.MonkeyPatch) -> list[Condition]:
    calls: list[Condition] = []
    original = Condition.ready

    def counting(self: Condition, machine: popcorn.Machine) -> bool:
        calls.append(self)
        return original(self, machine)

    monkeypatch.setattr(Condition, "ready", counting)
    return calls


def echoes(machine: popcorn.Machine, count: int) -> Condition:
    return machine.ref.var("log_len").ge(LOG_WORDS_PER_ECHO * count)


async def wait_parked(system: System, event: Condition | Conditions) -> popcorn.Stop:
    a, b = system.members["a"], system.members["b"]
    stop, _ = await asyncio.gather(a.wait_for(event, for_=ms(1)), b.run(for_=ms(1)))
    return stop


async def wait_running(system: System, event: Condition | Conditions) -> popcorn.Stop:
    async with system.running():
        return await system.members["a"].wait_for(event, for_=ms(1))


WAITS = [pytest.param(wait_parked, id="parked"), pytest.param(wait_running, id="running")]


@pytest.mark.parametrize("wait", WAITS)
def test_any_of_names_only_the_member_that_held_at_the_stop(
    ping: Callable[[], System],
    wait: Wait,
    ready_calls: list[Condition],
) -> None:
    system = ping()
    a = system.members["a"]
    first, second = echoes(a, 1), echoes(a, 2)
    stop = asyncio.run(wait(system, any_of(first, second)))
    assert stop.reason is StopReason.USER
    assert stop.fired == (first,)
    assert ready_calls == [first, second]


@pytest.mark.parametrize("wait", WAITS)
def test_all_of_names_every_member(ping: Callable[[], System], wait: Wait) -> None:
    system = ping()
    a = system.members["a"]
    first, second = echoes(a, 1), echoes(a, 2)
    stop = asyncio.run(wait(system, all_of(first, second)))
    assert stop.reason is StopReason.USER
    assert stop.fired == (first, second)


@pytest.mark.parametrize("wait", WAITS)
def test_a_never_true_condition_runs_no_python_per_event(
    ping: Callable[[], System],
    wait: Wait,
    ready_calls: list[Condition],
) -> None:
    system = ping()
    a = system.members["a"]
    stop = asyncio.run(wait(system, echoes(a, 1000)))
    assert stop.reason is StopReason.TIME
    assert stop.fired == ()
    assert ready_calls == []


def test_a_condition_of_another_member_is_refused(ping: Callable[[], System]) -> None:
    system = ping()
    a, b = system.members["a"], system.members["b"]
    with pytest.raises(ValueError, match="another machine"):
        asyncio.run(a.wait_for(echoes(b, 1), for_=ms(1)))


def test_two_waits_on_one_member_each_get_their_own_fired(ping: Callable[[], System]) -> None:
    system = ping()
    a = system.members["a"]
    first, second = echoes(a, 1), echoes(a, 2)

    async def body() -> tuple[popcorn.Stop, popcorn.Stop]:
        async with system.running():
            one = asyncio.create_task(a.wait_for(first, for_=ms(1)))
            two = asyncio.create_task(a.wait_for(second, for_=ms(1)))
            return await one, await two

    one, two = asyncio.run(body())
    assert one.reason is two.reason is StopReason.USER
    assert one.fired == (first,)
    assert two.fired == (second,)
    assert one.time < two.time


@pytest.mark.parametrize(
    ("field", "value", "match"),
    [
        ("size", 0, "1 to 8 bytes"),
        ("size", 9, "1 to 8 bytes"),
        ("bits", 0, "shift and bits"),
        ("shift", 60, "shift and bits"),
        ("where", 0x4000_0000, "plain memory"),
    ],
)
def test_a_raw_condition_predicate_validates_its_members(
    ping: Callable[[], System], field: str, value: int, match: str
) -> None:
    a = ping().members["a"]
    member = _core.MemoryCondition()
    member.where = 0x2000_8000
    member.size = 4
    member.shift = 0
    member.bits = 32
    setattr(member, field, value)
    when = _core.ConditionSet()
    when.members = [member]
    with pytest.raises(ValueError, match=match):
        a._handle.predicate_condition(when)  # pyright: ignore[reportPrivateUsage]


@pytest.mark.parametrize("count", [0, 65])
def test_a_raw_condition_predicate_takes_one_to_64_members(
    ping: Callable[[], System], count: int
) -> None:
    a = ping().members["a"]
    member = _core.MemoryCondition()
    member.where = 0x2000_8000
    member.size = 4
    member.shift = 0
    member.bits = 32
    when = _core.ConditionSet()
    when.members = [member] * count
    with pytest.raises(ValueError, match="1 to 64"):
        a._handle.predicate_condition(when)  # pyright: ignore[reportPrivateUsage]
