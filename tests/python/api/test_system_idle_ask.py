"""A wait on state an interrupt handler writes resolves when the core falls asleep after it, in a
linked system, at any slicing."""

from __future__ import annotations

import asyncio
from collections.abc import Callable

import pytest
from popcorn import System
from popcorn.units import VirtualDuration

from .linked import VARIANT_VALUES, VARIANTS, at, logged, past_done, to

Build = Callable[..., System]


async def wait_after_injected_byte(system: System, entries: int) -> tuple[int, list[int]]:
    b = system.members["b"]
    await to(system, at(3000))
    past_done(system)
    system.tap("b.USART1").inject(b"\x55", at=at(4000), towards="b.USART1")
    count = b.mem.u32[b.sym("log_len")]
    async with asyncio.timeout(5), system.running():
        await b.wait_for(b.ref.var("log_len").ge(count + entries))
    return b.time.ps, logged(b)[count:]


@pytest.mark.parametrize(("slice_instructions", "third"), VARIANTS)
@pytest.mark.parametrize("entries", [2, 4])
def test_a_wait_on_an_isr_written_value_resolves_at_the_sleep_instant(
    pair: Build, slice_instructions: int, third: VirtualDuration | None, entries: int
) -> None:
    at_ps, entries_logged = asyncio.run(
        wait_after_injected_byte(pair(slice_instructions, third), entries)
    )
    assert len(entries_logged) >= entries
    assert at_ps > at(4000).ps


@pytest.mark.parametrize("entries", [2, 4])
def test_the_resolving_instant_and_log_are_equal_at_every_slicing(
    pair: Build, entries: int
) -> None:
    results = [
        asyncio.run(wait_after_injected_byte(pair(slice_instructions, third), entries))
        for slice_instructions, third in VARIANT_VALUES
    ]
    assert results[1:] == results[:1] * (len(results) - 1)


def test_the_idle_entry_wait_ends_at_the_sleep_after_the_byte_entry_wait(pair: Build) -> None:
    byte_at, _ = asyncio.run(wait_after_injected_byte(pair(), 2))
    idle_at, _ = asyncio.run(wait_after_injected_byte(pair(), 4))
    assert idle_at > byte_at


async def back_to_back_waits(system: System, second: int) -> int:
    b = system.members["b"]
    await to(system, at(3000))
    past_done(system)
    system.tap("b.USART1").inject(b"\x55", at=at(4000), towards="b.USART1")
    count = b.mem.u32[b.sym("log_len")]
    async with asyncio.timeout(5), system.running():
        await b.wait_for(b.ref.var("log_len").ge(count + 4))
        await b.wait_for(b.ref.var("log_len").ge(count + second))
    return b.time.ps


@pytest.mark.parametrize("second", [2, 4])
def test_a_second_wait_already_true_while_the_member_sleeps_resolves(
    pair: Build, second: int
) -> None:
    once = asyncio.run(wait_after_injected_byte(pair(), 4))[0]
    assert asyncio.run(back_to_back_waits(pair(), second)) == once
