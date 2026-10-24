"""Cases of a `System` carried from the v1 plan: member stops and errors inside gathers,
halted members' peripherals, a pty waking a sleeping system, and Python models on several
member threads."""

from __future__ import annotations

import asyncio
import logging
import os
from collections.abc import Callable, Coroutine, Iterator
from typing import Any

import popcorn
import pytest
from popcorn import Stop, StopReason, System, UartLink
from popcorn.units import VirtualDuration, VirtualTime, ms, us

from .linked import Ticker, logged, member, ticker

L = us(20)
IRQ = "USART1_IRQHandler"
NEVER = "link_audio_peer"
VARIANTS: list[tuple[int, VirtualDuration | None]] = [
    (1_000_000, None),
    (997, None),
    (1_000_000, us(3)),
    (997, us(3)),
]
BAD_PC = 0xEFFF_FFF0
OVERRUN = 3
TX_BYTES = [0x20 + index for index in range(64)]

Build = Callable[..., System]
Models = dict[str, type[popcorn.model.Model]] | None


@pytest.fixture
def pair(u575: popcorn.PartDescription) -> Iterator[Build]:
    """Builds `a` running `a_entry` and `b` running `b_entry`, linked on `unit`, with a 3 us
    third link on USART3 when `third` is given."""
    made: list[System] = []

    def build(
        slice_instructions: int = 1_000_000,
        third: VirtualDuration | None = None,
        *,
        a_entry: str = "link_ping",
        b_entry: str = "link_echo",
        unit: str = "USART1",
        a_models: Models = None,
        b_models: Models = None,
    ) -> System:
        a = member(u575, a_entry, "a", slice_instructions, models=a_models)
        b = member(u575, b_entry, "b", slice_instructions, models=b_models)
        links = [UartLink(f"a.{unit}", f"b.{unit}", latency=L, baud=460_800)]
        if third is not None:
            links.append(UartLink("a.USART3", "b.USART3", latency=third, baud=460_800))
        system = System({"a": a, "b": b}, links, slice_instructions=slice_instructions)
        made.append(system)
        return system

    yield build
    for one in made:
        one.close()


def across(
    pair: Build,
    body: Callable[[System], Coroutine[Any, Any, object]],
    variants: list[tuple[int, VirtualDuration | None]] | None = None,
    **options: Any,
) -> Any:
    """`body`'s result on a fresh system in every variant, checked equal, and returned."""
    results: list[object] = []
    for slice_instructions, third in variants or VARIANTS:
        system = pair(slice_instructions, third, **options)
        results.append(asyncio.run(body(system)))
    assert results[1:] == results[:1] * (len(results) - 1)
    return results[0]


def point(stop: Stop) -> tuple[object, ...]:
    return (stop.reason, stop.pc, stop.instructions, stop.time)


def test_a_fault_ends_its_member_s_goal_and_the_peer_reaches_its_own(
    u575: popcorn.PartDescription, pair: Build
) -> None:
    async def body(system: System) -> object:
        a, b = system.members["a"], system.members["b"]
        a.reg.pc = BAD_PC
        a_stop, b_stop = await asyncio.gather(a.run(for_=ms(1)), b.run(for_=ms(1)))
        assert a_stop.reason is StopReason.FAULT and b_stop.reason is StopReason.TIME
        return point(a_stop), a_stop.fault, point(b_stop)

    a_point, fault, _ = across(pair, body)
    lone = member(u575, "link_ping", "a")
    lone.reg.pc = BAD_PC
    alone = asyncio.run(lone.run(for_=ms(1)))
    assert (point(alone), alone.fault) == (a_point, fault)
    assert fault is not None


def test_a_budget_ends_its_member_s_goal_as_on_a_lone_machine(
    u575: popcorn.PartDescription, pair: Build
) -> None:
    async def body(system: System) -> object:
        a, b = system.members["a"], system.members["b"]
        async with asyncio.timeout(10):
            a_stop, b_stop = await asyncio.gather(
                a.run(until=NEVER, budget=10_000, for_=ms(1)), b.run(for_=ms(1))
            )
        assert a_stop.reason is StopReason.BUDGET and b_stop.reason is StopReason.TIME
        return point(a_stop), point(b_stop)

    a_point, _ = across(pair, body)
    lone = member(u575, "link_ping", "a")
    alone = asyncio.run(lone.run(until=NEVER, budget=10_000, for_=ms(1)))
    assert point(alone)[:3] == a_point[:3]


def test_goals_of_a_sleeping_system_each_resolve_idle(pair: Build) -> None:
    async def body(system: System) -> object:
        a, b = system.members["a"], system.members["b"]
        async with asyncio.timeout(10):
            a_stop, b_stop = await asyncio.gather(a.run(until=NEVER), b.run(until=NEVER))
        assert a_stop.reason is StopReason.IDLE and b_stop.reason is StopReason.IDLE
        assert system.time_of("a") == system.time_of("b")
        return point(a_stop), point(b_stop), system.time_of("a")

    across(pair, body, a_entry="idle", b_entry="idle")


def test_every_pending_goal_fails_with_a_member_s_error_which_names_it(pair: Build) -> None:
    async def body(system: System) -> object:
        a, b = system.members["a"], system.members["b"]
        ticker(a).raise_at = 50
        results = await asyncio.gather(a.run(for_=ms(2)), b.run(for_=ms(1)), return_exceptions=True)
        assert results[0] is results[1]
        error = results[0]
        assert isinstance(error, RuntimeError)
        with pytest.raises(popcorn.EmulationError, match="stopped on an error") as again:
            await system.run(for_=ms(1))
        assert again.value.__cause__ is error
        return str(error), error.__notes__

    message, notes = across(pair, body, a_models={"TIM6": Ticker})
    assert message == "TIM6 tick 50"
    assert notes == ["raised by member a at system time t=100500000 ps"]


def test_a_goal_resolved_before_a_member_s_error_keeps_its_stop(pair: Build) -> None:
    async def body(system: System) -> object:
        a, b = system.members["a"], system.members["b"]
        ticker(a).raise_at = 50
        results = await asyncio.gather(
            a.run(for_=ms(2)), b.run(for_=us(50)), return_exceptions=True
        )
        assert isinstance(results[0], RuntimeError)
        assert isinstance(results[1], Stop) and results[1].reason is StopReason.TIME
        return str(results[0]), point(results[1])

    across(pair, body, a_models={"TIM6": Ticker})


def test_a_task_group_fails_with_a_member_s_error_in_every_goal(pair: Build) -> None:
    async def body(system: System) -> object:
        a, b = system.members["a"], system.members["b"]
        ticker(a).raise_at = 50
        with pytest.raises(ExceptionGroup) as raised:
            async with asyncio.TaskGroup() as group:
                group.create_task(a.run(for_=ms(2)))
                group.create_task(b.run(for_=ms(5)))
        errors = raised.value.exceptions
        assert [type(error) for error in errors] == [RuntimeError, RuntimeError]
        assert errors[0] is errors[1]
        return str(errors[0]), errors[0].__notes__

    across(pair, body, a_models={"TIM6": Ticker})


def test_a_halted_member_s_error_fails_the_goal_of_the_next_run(pair: Build) -> None:
    async def body(system: System) -> object:
        a, b = system.members["a"], system.members["b"]
        ticker(a).raise_at = 150
        first = await a.run(for_=us(100))
        with pytest.raises(RuntimeError, match="TIM6 tick 150") as raised:
            await b.run(for_=ms(3))
        return point(first), raised.value.__notes__

    _, notes = across(pair, body, a_models={"TIM6": Ticker})
    assert notes == ["raised by member a at system time t=1100500000 ps"]


def test_a_member_s_goals_count_only_what_it_executed_not_its_halt(pair: Build) -> None:
    async def body(system: System) -> object:
        a, b = system.members["a"], system.members["b"]
        first = await a.run(for_=us(300))
        before = system._handle.executed_of(0)  # pyright: ignore[reportPrivateUsage]
        await b.run(for_=ms(2))
        halted = system._handle.executed_of(0)  # pyright: ignore[reportPrivateUsage]
        second = await a.run(until=NEVER, budget=5_000)
        after = system._handle.executed_of(0)  # pyright: ignore[reportPrivateUsage]
        assert halted == before
        assert second.reason is StopReason.BUDGET
        return point(first), point(second), after - halted

    _, _, executed = across(pair, body)
    assert 5_000 <= executed < 5_010


def test_gather_order_lone_goals_and_halted_sequences_are_exact(pair: Build) -> None:
    async def gathered(system: System, reverse: bool) -> object:
        a, b = system.members["a"], system.members["b"]
        if reverse:
            b_stop, a_stop = await asyncio.gather(b.run(for_=ms(1)), a.run(until=IRQ))
        else:
            a_stop, b_stop = await asyncio.gather(a.run(until=IRQ), b.run(for_=ms(1)))
        return point(a_stop), point(b_stop), system.time_of("a"), system.time_of("b")

    async def forward(system: System) -> object:
        return await gathered(system, False)

    async def backward(system: System) -> object:
        return await gathered(system, True)

    assert across(pair, forward) == across(pair, backward)

    async def sequence(system: System) -> object:
        a, b = system.members["a"], system.members["b"]
        first = await a.run(for_=us(300))
        assert a.time == first.time
        assert system.time_of("a") == system.time_of("b")
        second = await b.run(for_=ms(1))
        third = await a.step(5)
        assert third.reason is StopReason.BUDGET
        return point(first), point(second), point(third), logged(b)

    across(pair, sequence)


def test_a_halted_receiver_s_usart_overruns_and_takes_its_interrupt_on_its_next_goal(
    pair: Build,
) -> None:
    async def body(system: System) -> object:
        a, b = system.members["a"], system.members["b"]
        await b.run(for_=us(700))
        before = len(logged(b))
        await a.run(for_=us(500))
        assert len(logged(b)) == before
        assert b.periph.USART1.ISR_disabled.ORE == 1
        stop = await b.step()
        assert stop.location.symbol == IRQ
        await b.run(for_=us(100))
        kinds = [word >> 16 for word in logged(b)[::2]]
        assert OVERRUN in kinds
        return point(stop), logged(b)

    across(pair, body, a_entry="link_tx", b_entry="link_rx")


@pytest.mark.parametrize("frozen", [False, True])
def test_a_halted_receiver_s_dma_ring_fills_unless_its_channel_is_frozen(
    pair: Build, frozen: bool
) -> None:
    async def body(system: System) -> object:
        a, b = system.members["a"], system.members["b"]
        if frozen:
            b.periph.DBGMCU.AHB1FZR.DBG_GPDMA3_STOP = 1
        await b.run(for_=us(400))
        done = await a.run(for_=ms(5))
        assert done.reason is StopReason.USER and a.mem.u16[a.pc - 2] == 0xBE01
        a.reg.pc = a.sym("idle")
        await a.run(for_=ms(1))
        return bytes(b.mem.u8[b.sym("big_ring") + index] for index in range(64))

    ring = across(pair, body, a_entry="link_tx", b_entry="link_rx_dma")
    assert (ring == bytes(TX_BYTES)) is not frozen


@pytest.mark.parametrize("frozen", [False, True])
def test_a_halted_sender_s_watchdog_bites_unless_it_is_frozen(
    pair: Build, frozen: bool, caplog: pytest.LogCaptureFixture
) -> None:
    async def body(system: System) -> object:
        a, b = system.members["a"], system.members["b"]
        if frozen:
            a.periph.DBGMCU.APB1LFZR.DBG_IWDG_STOP = 1
        await a.run(for_=ms(1))
        base = system.to_system("a", a.time)
        caplog.clear()
        with caplog.at_level(logging.WARNING, logger="popcorn"):
            await b.run(for_=ms(9))
        bites = [r for r in caplog.records if "while halted" in r.getMessage()]
        assert len(bites) == (0 if frozen else 1)
        return a.pc, a.time, system.to_system("a", a.time) == base

    across(pair, body, a_entry="link_iwdg", b_entry="link_rx")


async def sleeps_reach(system: System, count: int) -> None:
    """Let the loop turn until the members went to sleep `count` times in all, under a 10 s
    guard against a hang."""
    async with asyncio.timeout(10):
        while system._handle.sleeps() < count:  # pyright: ignore[reportPrivateUsage]
            await asyncio.sleep(0.001)


def pty_woken(
    u575: popcorn.PartDescription, slice_instructions: int, third: VirtualDuration | None
) -> tuple[tuple[int, int], list[int], tuple[object, ...]]:
    """Start a system of an idle `a` and a `b` receiving on a pty, write one byte once both
    sleep, halt once both sleep again: the sleep counts, b's log, the members' positions."""
    a = member(u575, "idle", "a", slice_instructions)
    b = member(u575, "link_rx", "b", slice_instructions)
    path = b.board.pty("USART1")
    links = [UartLink("a.USART2", "b.USART2", latency=L, baud=460_800)]
    if third is not None:
        links.append(UartLink("a.USART3", "b.USART3", latency=third, baud=460_800))
    fd = os.open(path, os.O_RDWR | os.O_NOCTTY)
    try:
        with System({"a": a, "b": b}, links, slice_instructions=slice_instructions) as system:

            async def body() -> tuple[int, int]:
                system.start()
                await sleeps_reach(system, 2)
                asleep = system._handle.sleeps()  # pyright: ignore[reportPrivateUsage]
                os.write(fd, b"Q")
                await sleeps_reach(system, asleep + 2)
                again = system._handle.sleeps()  # pyright: ignore[reportPrivateUsage]
                system.halt()
                return asleep, again

            counts = asyncio.run(body())
            return counts, logged(b), (system.time_of("a"), system.time_of("b"), a.pc, b.pc)
    finally:
        os.close(fd)


def test_a_pty_byte_wakes_a_sleeping_system_at_the_same_point_at_any_slicing(
    u575: popcorn.PartDescription,
) -> None:
    results = [pty_woken(u575, slice_instructions, third) for slice_instructions, third in VARIANTS]
    assert results[1:] == results[:1] * 3
    counts, log, _ = results[0]
    assert counts == (2, 4)
    assert log[0] == (1 << 16) | ord("Q")


def test_python_models_on_two_members_run_deterministically_one_gil_entry_per_call(
    pair: Build,
) -> None:
    def ports(m: popcorn.Machine, words: int) -> list[int]:
        return [m.mem.u32[m.sym("audio_ports") + 4 * index] for index in range(words)]

    async def body(system: System) -> object:
        a, b = system.members["a"], system.members["b"]
        ticker(a).period_us = ticker(b).period_us = 20
        entries = [m._handle.gil_entries() for m in (a, b)]  # pyright: ignore[reportPrivateUsage]
        booted = [ticker(m).ticks for m in (a, b)]
        stop = await system.run(for_=ms(100))
        ticks = [ticker(m).ticks - before for m, before in zip((a, b), booted, strict=True)]
        assert [
            m._handle.gil_entries() - before  # pyright: ignore[reportPrivateUsage]
            for m, before in zip((a, b), entries, strict=True)
        ] == ticks
        return (
            stop.reason,
            [point(one) for one in stop.stops.values()],
            ticks,
            ports(a, 14),
            ports(b, 7),
        )

    options: dict[str, Any] = {
        "a_entry": "link_audio",
        "b_entry": "link_audio_peer",
        "a_models": {"TIM6": Ticker},
        "b_models": {"TIM6": Ticker},
    }
    reason, _, ticks, a_ports, _ = across(pair, body, **options)
    assert reason is StopReason.TIME
    assert min(ticks) > 4_000
    assert a_ports[4] == a_ports[11] == 6

    async def long(system: System) -> object:
        for m in system.members.values():
            ticker(m).period_us = 20
        stop = await system.run(for_=ms(1000))
        return stop.reason, [point(one) for one in stop.stops.values()]

    reason, _ = across(pair, long, [(1_000_000, None), (1_000_000, None)], **options)
    assert reason is StopReason.TIME


async def to(system: System, until: VirtualTime) -> None:
    """Run to `until`, again after each member stop."""
    async with asyncio.timeout(10):
        while (await system.run(until=until)).reason is not StopReason.TIME:
            pass


def test_a_watchdog_reset_of_a_running_member_lets_the_system_run_on(pair: Build) -> None:
    async def body(system: System) -> object:
        a = system.members["a"]
        boot = a.time
        await to(system, VirtualTime(ms(8).ps))
        base = system.time_of("a").ps - a.time.ps
        assert base > -boot.ps
        frames = system.tap("a.USART1").frames()
        assert sum(frame.lost for frame in frames) == 1
        return base, system.time_of("a"), frames

    across(pair, body, a_entry="link_iwdg", b_entry="link_rx")


def test_an_armed_replace_taken_in_a_snapshot_applies_again_after_a_restore(pair: Build) -> None:
    async def body(system: System) -> object:
        b = system.members["b"]
        tap = system.tap("a.USART1")
        tap.replace(40, b"abc", towards="b.USART1")
        taken = system.snapshot()
        await to(system, VirtualTime(ms(3).ps))
        first = logged(b), tap.frames()
        system.restore(taken)
        await to(system, VirtualTime(ms(3).ps))
        assert (logged(b), tap.frames()) == first
        return first

    log, frames = across(pair, body, a_entry="link_tx", b_entry="link_rx")
    expected = list(TX_BYTES)
    expected[40:43] = list(b"abc")
    assert [frame.data for frame in frames] == expected
    assert [word & 0xFFFF for word in log[::2] if word >> 16 == 1] == expected
