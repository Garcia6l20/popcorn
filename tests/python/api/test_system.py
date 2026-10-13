"""`popcorn.LinkedSystem`: members on one virtual clock, their UARTs linked."""

from __future__ import annotations

import asyncio
import gc
import itertools
import logging
import threading
import weakref
from collections.abc import Callable, Iterator
from typing import Any, Literal, cast

import popcorn
import pytest
from popcorn import LinkedSystem, StopReason, SystemStop, UartLink
from popcorn.units import VirtualDuration, VirtualTime, ms, s, us

from .linked import PING_BYTES, Ticker, logged, member, received, ticker

L = us(20)
VARIANTS = [
    pytest.param(1_000_000, None, id="plain"),
    pytest.param(997, None, id="997"),
    pytest.param(1_000_000, us(3), id="q=3us"),
    pytest.param(997, us(3), id="997 q=3us"),
]

Build = Callable[..., LinkedSystem]


def links(third: VirtualDuration | None) -> list[UartLink]:
    made = [UartLink("a.USART1", "b.USART1", latency=L, baud=460_800)]
    if third is not None:
        made.append(UartLink("a.USART2", "b.USART2", latency=third, baud=460_800))
    return made


@pytest.fixture
def ping(u575: popcorn.PartDescription) -> Iterator[Build]:
    made: list[LinkedSystem] = []

    def build(
        slice_instructions: int = 1_000_000,
        third: VirtualDuration | None = None,
        *,
        b_entry: str = "link_echo",
        **options: object,
    ) -> LinkedSystem:
        a = member(u575, "link_ping", "a", slice_instructions)
        b = member(u575, b_entry, "b", slice_instructions)
        system = LinkedSystem(
            {"a": a, "b": b},
            links(third),
            slice_instructions=slice_instructions,
            **options,  # pyright: ignore[reportArgumentType]
        )
        made.append(system)
        return system

    yield build
    for one in made:
        one.close()


def state(system: LinkedSystem) -> tuple[object, ...]:
    members = system.members
    return tuple(
        (
            tuple(logged(m)),
            m.pc,
            m.instructions,
            m.time,
            system.time_of(name),
        )
        for name, m in members.items()
    )


def test_a_run_reaches_its_target_on_every_member(ping: Build) -> None:
    system = ping()
    assert system.quantum == L
    first = asyncio.run(system.run(for_=ms(3)))
    assert (first.reason, first.member) == (StopReason.USER, "a")
    assert first.stop is not None and first.stop.reason is StopReason.USER
    assert set(first.stops) == {"a"}
    done = asyncio.run(system.run(until=VirtualTime(ms(3).ps)))
    assert done.reason is StopReason.TIME
    assert done.time == VirtualTime(ms(3).ps)
    assert set(done.stops) == {"a", "b"}
    for name in ("a", "b"):
        assert system.time_of(name) == VirtualTime(ms(3).ps)
        assert done.stops[name].reason is StopReason.TIME
    assert received(system.members["a"]) == [0x40 + index for index in range(PING_BYTES)]
    assert system.time >= VirtualTime(ms(3).ps)


async def _to(system: LinkedSystem, until: VirtualDuration) -> list[SystemStop]:
    """Run to system time `until`, again after each member stop, until the TIME stop."""
    stops = [await system.run(until=VirtualTime(until.ps))]
    while stops[-1].reason is not StopReason.TIME:
        stops.append(await system.run(until=VirtualTime(until.ps)))
    return stops


@pytest.mark.parametrize(("slice_instructions", "third"), VARIANTS)
def test_chunked_runs_equal_one_run(
    ping: Build, slice_instructions: int, third: VirtualDuration | None
) -> None:
    whole = ping(slice_instructions, third)
    one = asyncio.run(_to(whole, ms(4)))
    chunked = ping(slice_instructions, third)
    many: list[SystemStop] = []
    for step in range(1, 9):
        many.extend(asyncio.run(_to(chunked, VirtualDuration(step * ms(0.5).ps))))
    members = [(stop.reason, stop.member, stop.time) for stop in one if stop.member is not None]
    assert members == [(stop.reason, stop.member, stop.time) for stop in many if stop.member]
    assert state(chunked) == state(whole)
    assert asyncio.run(chunked.run(for_=ms(1))).time == VirtualTime(ms(5).ps)


def test_the_same_run_at_any_slicing_or_quantum(ping: Build) -> None:
    states: list[tuple[object, ...]] = []
    for slice_instructions, third in [
        (1_000_000, None),
        (997, None),
        (1_000_000, us(3)),
        (997, us(3)),
    ]:
        system = ping(slice_instructions, third)
        stops = asyncio.run(_to(system, ms(4)))
        states.append((tuple((one.reason, one.member, one.time) for one in stops), state(system)))
    assert states[1:] == states[:1] * 3


def test_offsets_fix_each_member_s_system_time(ping: Build) -> None:
    system = ping(offsets={"b": us(100)})
    a, b = system.members["a"], system.members["b"]
    a_start, b_start = a.time.ps, b.time.ps
    assert system.time_of("a") == VirtualTime(0)
    assert system.time_of("b") == VirtualTime(us(100).ps)
    stop = asyncio.run(_to(system, ms(4)))[-1]
    assert stop.reason is StopReason.TIME
    assert system.time_of("a").ps == a.time.ps - a_start >= ms(4).ps
    assert system.time_of("b").ps == b.time.ps - b_start + us(100).ps >= ms(4).ps
    assert stop.stops["b"].time.ps == b.time.ps


def test_construction_checks(u575: popcorn.PartDescription) -> None:
    a = member(u575, "link_ping", "a")
    b = member(u575, "link_echo", "b")
    good = [UartLink("a.USART1", "b.USART1", latency=L, baud=460_800)]
    cases: list[tuple[Callable[[], object], str]] = [
        (lambda: LinkedSystem({"x": a, "b": b}, good), "members.x"),
        (lambda: LinkedSystem({"a": a, "b": b}, []), "links"),
        (
            lambda: LinkedSystem(
                {"a": a, "b": b}, [UartLink("a", "b.USART1", latency=L, baud=460_800)]
            ),
            "links[0].a",
        ),
        (
            lambda: LinkedSystem(
                {"a": a, "b": b}, [UartLink("a.USART1", "c.USART1", latency=L, baud=460_800)]
            ),
            "links[0].b",
        ),
        (
            lambda: LinkedSystem(
                {"a": a, "b": b}, [UartLink("a.USART1", "b.GPIOA", latency=L, baud=460_800)]
            ),
            "links[0].b",
        ),
        (
            lambda: LinkedSystem(
                {"a": a, "b": b},
                [*good, UartLink("a.USART1", "b.USART2", latency=L, baud=460_800)],
            ),
            "links[1].a",
        ),
        (
            lambda: LinkedSystem(
                {"a": a, "b": b},
                [UartLink("a.USART1", "b.USART1", latency=VirtualDuration(0), baud=460_800)],
            ),
            "links[0].latency",
        ),
        (
            lambda: LinkedSystem(
                {"a": a, "b": b},
                [UartLink("a.USART1", "b.USART1", latency=L, baud=460_800, stop_bits=3)],
            ),
            "links[0].stop_bits",
        ),
        (
            lambda: LinkedSystem(
                {"a": a, "b": b}, [UartLink("a.USART1", "b.USART1", latency=L, baud=0)]
            ),
            "links[0].baud",
        ),
        (
            lambda: LinkedSystem(
                {"a": a, "b": b},
                [
                    UartLink(
                        "a.USART1", "b.USART1", latency=L, baud=460_800, data_bits=9, parity="even"
                    )
                ],
            ),
            "links[0].data_bits",
        ),
        (
            lambda: LinkedSystem(
                {"a": a, "b": b},
                [
                    UartLink(
                        "a.USART1", "b.USART1", latency=L, baud=460_800, parity=cast(Any, "mark")
                    )
                ],  # pyright: ignore[reportArgumentType]
            ),
            "links[0].parity",
        ),
        (lambda: LinkedSystem({"a": a, "b": b}, good, offsets={"c": us(1)}), "offsets.c"),
        (
            lambda: LinkedSystem({"a": a, "b": b}, good, offsets={"a": VirtualDuration(-1)}),
            "offsets.a",
        ),
    ]
    for build, key in cases:
        with pytest.raises(popcorn.ConfigError) as raised:
            build()
        assert raised.value.key == key
    with pytest.raises(ValueError, match="above 0"):
        LinkedSystem({"a": a, "b": b}, good, slice_instructions=0)
    b.board.pty("USART1")
    with pytest.raises(popcorn.ConfigError, match="bridged to a pty") as raised:
        LinkedSystem({"a": a, "b": b}, good)
    assert raised.value.key == "links[0].b"
    with LinkedSystem(
        {"a": a, "b": b}, [UartLink("a.USART1", "b.USART2", latency=L, baud=460_800)]
    ) as system:
        with pytest.raises(popcorn.ConfigError, match="another linked system"):
            LinkedSystem(
                {"a": a, "b": b}, [UartLink("a.USART3", "b.USART3", latency=L, baud=460_800)]
            )
        assert system.members["a"] is a
    with LinkedSystem(
        {"a": a, "b": b}, [UartLink("a.USART3", "b.USART3", latency=L, baud=460_800)]
    ) as last:
        assert last.quantum == L


def test_a_member_runs_and_saves_only_through_its_system(ping: Build) -> None:
    system = ping()
    a = system.members["a"]
    with pytest.raises(popcorn.MachineBusy, match="running"):
        a.start()
    with pytest.raises(popcorn.MachineBusy, match="member of a linked system"):
        a.snapshot()
    with pytest.raises(popcorn.MachineBusy, match="use the system's restore"):
        a.restore(cast(Any, None))
    system.close()
    system.close()
    with pytest.raises(popcorn.EmulationError, match="closed"):
        asyncio.run(system.run(for_=us(10)))
    a.snapshot()
    assert asyncio.run(a.run(for_=us(10))).reason is StopReason.TIME


def test_accessors_raise_while_the_system_runs(ping: Build) -> None:
    system = ping(b_entry="link_rx")

    async def body() -> None:
        task = asyncio.create_task(system.run(for_=s(100)))
        await asyncio.sleep(0)
        with pytest.raises(popcorn.MachineRunning):
            _ = system.time
        with pytest.raises(popcorn.MachineRunning):
            system.time_of("a")
        with pytest.raises(popcorn.MachineRunning):
            _ = system.members["b"].pc
        with pytest.raises(popcorn.MachineBusy):
            await system.run(for_=ms(1))
        task.cancel()
        with pytest.raises(asyncio.CancelledError):
            await task

    asyncio.run(body())
    assert system.members["b"].pc != 0


def test_a_cancelled_run_pauses_and_the_next_run_goes_on(ping: Build) -> None:
    system = ping(b_entry="link_rx")

    async def body() -> SystemStop:
        with pytest.raises(TimeoutError):
            await asyncio.wait_for(system.run(for_=s(100)), 0.05)
        return await system.run(for_=ms(1))

    stop = asyncio.run(body())
    assert stop.reason in (StopReason.TIME, StopReason.USER)


def test_a_strict_shape_mismatch_stops_the_system_with_its_error(
    u575: popcorn.PartDescription,
) -> None:
    a = member(u575, "link_ping", "a")
    b = member(u575, "link_echo", "b", strict=True)
    with LinkedSystem(
        {"a": a, "b": b}, [UartLink("a.USART1", "b.USART1", latency=L, baud=115_200)]
    ) as system:
        with pytest.raises(popcorn.ModelError):
            asyncio.run(system.run(for_=ms(1)))
        with pytest.raises(popcorn.EmulationError, match="stopped on an error"):
            asyncio.run(system.run(for_=ms(1)))


def test_a_member_s_handler_error_stops_the_system_and_names_the_member(
    u575: popcorn.PartDescription,
) -> None:
    a = member(u575, "link_ping", "a", models={"TIM6": Ticker})
    b = member(u575, "link_echo", "b")
    ticker(a).raise_at = 50
    with LinkedSystem({"a": a, "b": b}, links(None)) as system:
        with pytest.raises(RuntimeError, match="TIM6 tick 50") as raised:
            asyncio.run(system.run(for_=ms(1)))
        assert raised.value.__notes__ == ["raised by member a at system time t=100500000 ps"]
        with pytest.raises(popcorn.EmulationError, match="stopped on an error") as again:
            asyncio.run(system.run(for_=ms(1)))
        assert again.value.__cause__ is raised.value
        assert raised.value.__notes__ == ["raised by member a at system time t=100500000 ps"]


def test_a_closed_system_and_its_members_are_freed(u575: popcorn.PartDescription) -> None:
    a = member(u575, "link_ping", "a")
    b = member(u575, "link_echo", "b")
    system = LinkedSystem({"a": a, "b": b}, links(None))
    asyncio.run(system.run(for_=ms(1)))
    held = weakref.ref(system), weakref.ref(a), weakref.ref(b)
    system.close()
    del system, a, b
    gc.collect()
    assert [one() for one in held] == [None, None, None]


@pytest.mark.parametrize(("slice_instructions", "third"), VARIANTS)
def test_a_close_refused_from_a_callback_leaves_the_system_open(
    ping: Build, slice_instructions: int, third: VirtualDuration | None
) -> None:
    system = ping(slice_instructions, third)
    a, b = system.members["a"], system.members["b"]
    refused: list[str] = []

    def close_inside(call: popcorn.Call) -> popcorn.Return:
        try:
            system.close()
        except RuntimeError as error:
            refused.append(str(error))
        return call.proceed()

    b.intercept("USART1_IRQHandler", close_inside, once=True)
    assert asyncio.run(system.run(for_=ms(1))).reason is StopReason.TIME
    assert refused == ["a linked system is not driven from a callback of a machine"]
    assert asyncio.run(system.run(for_=ms(1))).reason is StopReason.TIME
    system.close()
    assert asyncio.run(a.run(for_=us(10))).reason is StopReason.TIME
    assert asyncio.run(b.run(for_=us(10))).reason is StopReason.TIME


def test_types_are_checked_at_the_call(ping: Build) -> None:
    system = ping()
    with pytest.raises(TypeError, match="for_ or until"):
        asyncio.run(system.run())
    with pytest.raises(TypeError, match="VirtualDuration"):
        asyncio.run(system.run(for_=1000))  # pyright: ignore[reportArgumentType]
    with pytest.raises(TypeError, match="VirtualTime"):
        asyncio.run(system.run(until=ms(1)))  # pyright: ignore[reportArgumentType]
    with pytest.raises(ValueError, match="negative"):
        asyncio.run(system.run(for_=VirtualDuration(-1)))


def test_for_counts_from_the_last_target_not_from_a_member_past_it(
    u575: popcorn.PartDescription,
) -> None:
    def build() -> LinkedSystem:
        a = member(u575, "link_tx_count", "a")
        b = member(u575, "link_rx", "b")
        return LinkedSystem({"a": a, "b": b}, links(None))

    with build() as chunked, build() as whole:
        first = asyncio.run(chunked.run(for_=us(100)))
        assert first.reason is StopReason.TIME
        assert chunked.time_of("a") > first.time
        second = asyncio.run(chunked.run(for_=us(100)))
        assert second.time == VirtualTime(us(200).ps)
        once = asyncio.run(whole.run(for_=us(200)))
        assert once.time == second.time
        assert state(chunked) == state(whole)


def test_for_after_a_member_stop_counts_from_the_construction_origin(ping: Build) -> None:
    seen: list[tuple[object, ...]] = []
    for slice_instructions, third in [
        (1_000_000, None),
        (997, None),
        (1_000_000, us(3)),
        (997, us(3)),
    ]:
        system = ping(slice_instructions, third)
        first = asyncio.run(system.run(for_=ms(3)))
        assert (first.reason, first.member) == (StopReason.USER, "a")
        after = asyncio.run(system.run(for_=ms(3)))
        assert after.reason is StopReason.TIME
        assert after.time == VirtualTime(ms(3).ps)
        seen.append((after.time, state(system)))
    assert seen[1:] == seen[:1] * 3


def test_a_cancel_while_waiting_for_the_last_pause_reaches_the_caller(ping: Build) -> None:
    system = ping()
    gate = threading.Event()
    calls: list[int] = []

    def held() -> bool:
        calls.append(1)
        gate.wait(10)
        return False

    handle = system._handle  # pyright: ignore[reportPrivateUsage]
    goal = handle.post_goal(1, None, None, None, s(100).ps, held, None, False, None)

    async def body() -> None:
        first = asyncio.create_task(system.run(for_=s(100)))
        while not calls:
            await asyncio.sleep(0.001)
        first.cancel()
        with pytest.raises(asyncio.CancelledError):
            await first
        second = asyncio.create_task(system.run(for_=ms(1)))
        await asyncio.sleep(0)
        await asyncio.sleep(0)
        second.cancel()
        gate.set()
        with pytest.raises(asyncio.CancelledError):
            await second
        while system._flight is not None:  # pyright: ignore[reportPrivateUsage]
            await asyncio.sleep(0.001)

    asyncio.run(body())
    assert system.time >= VirtualTime(0)
    handle.withdraw(1)
    del goal


PARITY_CALLS = itertools.count()


def parity_warnings(
    u575: popcorn.PartDescription,
    caplog: pytest.LogCaptureFixture,
    declared: Literal["none", "even", "odd"],
    odd: tuple[bool, bool] | None,
) -> list[str]:
    """The link shape warnings of a USART2 link declared with `declared` parity, each end
    sending one frame with PCE set and PS = `odd[i]`, or without parity for None. Members get
    names of their own, so warnings another system forwards late are not counted."""
    call = next(PARITY_CALLS)
    names = (f"pa{call}", f"pb{call}")
    a = member(u575, "idle", names[0])
    b = member(u575, "idle", names[1])
    caplog.clear()
    with LinkedSystem(
        {names[0]: a, names[1]: b},
        [
            UartLink(
                f"{names[0]}.USART2",
                f"{names[1]}.USART2",
                latency=L,
                baud=115_200,
                parity=declared,
            )
        ],
    ) as system:
        for index, m in enumerate((a, b)):
            m.periph.RCC.RCC_APB1ENR1.USART2EN = 1
            uart = m.periph.USART2
            uart.BRR.value = 35
            parity = {} if odd is None else {"PCE": 1, "M0": 1, "PS": int(odd[index])}
            uart.CR1_disabled.poke(UE=1, TE=1, RE=1, **parity)
            uart.TDR.value = 0x41
        with caplog.at_level(logging.WARNING, logger="popcorn"):
            assert asyncio.run(system.run(for_=ms(1))).reason is StopReason.TIME
    return [
        r.getMessage()
        for r in caplog.records
        if r.getMessage().startswith(tuple(f"{name}:" for name in names))
        and "on a link declared" in r.getMessage()
    ]


def test_a_link_s_parity_reaches_both_directions(
    u575: popcorn.PartDescription, caplog: pytest.LogCaptureFixture
) -> None:
    assert parity_warnings(u575, caplog, "none", None) == []
    assert parity_warnings(u575, caplog, "even", (False, False)) == []
    assert parity_warnings(u575, caplog, "odd", (True, True)) == []
    mismatched = parity_warnings(u575, caplog, "even", (True, True))
    assert len(mismatched) == 2
    assert all("parity odd on a link declared" in one for one in mismatched)
    assert all(one.endswith("parity even") for one in mismatched)
    plain = parity_warnings(u575, caplog, "even", None)
    assert len(plain) == 2
    assert all("8 data bits" in one and "9 data bits" in one for one in plain)


def _self_linked(
    u575: popcorn.PartDescription, slice_instructions: int, third: VirtualDuration | None
) -> tuple[object, ...]:
    a = member(u575, "link_self", "a", slice_instructions)
    b = member(u575, "idle", "b", slice_instructions)
    made = [
        UartLink("a.USART1", "a.USART3", latency=L, baud=460_800),
        UartLink("b.USART1", "a.USART2", latency=L, baud=460_800),
    ]
    if third is not None:
        made.append(UartLink("b.USART2", "a.UART4", latency=third, baud=460_800))
    with LinkedSystem({"a": a, "b": b}, made, slice_instructions=slice_instructions) as system:
        stop = asyncio.run(system.run(for_=ms(3)))
        assert (stop.reason, stop.member) == (StopReason.USER, "a")
        frames = system.tap("a.USART1").frames()
        assert received(a) == [0x40 + index for index in range(PING_BYTES)]
        assert [frame.towards for frame in frames[:4]] == ["a.USART3", "a.USART1"] * 2
        for frame in frames:
            assert frame.arrival.ps == frame.end.ps + L.ps
        sent = [frame.data for frame in frames if frame.towards == "a.USART3"]
        echoed = [frame.data for frame in frames if frame.towards == "a.USART1"]
        assert sent == echoed == received(a)
        return (tuple(logged(a)), a.pc, a.instructions, a.time, stop.time, tuple(frames))


def test_a_link_between_two_uarts_of_one_member_runs_as_any_link(
    u575: popcorn.PartDescription,
) -> None:
    results = [
        _self_linked(u575, slice_instructions, third)
        for slice_instructions, third in (
            (1_000_000, None),
            (997, None),
            (1_000_000, us(3)),
            (997, us(3)),
        )
    ]
    assert results[1:] == results[:-1]
