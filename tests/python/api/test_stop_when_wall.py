"""A parked stop_when is asked after each dispatched event and at instruction-count slice ends,
never at a slice end the wall clock, a host thread or a mail post sets."""

from __future__ import annotations

import asyncio

import popcorn
import pytest
from popcorn import StopReason

from .linked import member

STOP_TIME_PS = 644_750_000
STOP_INSTRUCTIONS = 2579


def tx_next_ge_3(m: popcorn.Machine) -> bool:
    return int(m.var("tx_next").value) >= 3


@pytest.mark.parametrize("budget", [1e-9, 1e-3, 60.0])
def test_a_parked_condition_wait_stops_at_the_same_point_at_any_wall_budget(
    u575: popcorn.PartDescription, budget: float
) -> None:
    m = member(u575, "tx_irq", "a", slice_wall_budget=budget)
    stop = asyncio.run(m.wait_for(m.ref.var("tx_next").ge(3)))
    assert stop.reason is StopReason.USER
    assert stop.time.ps == STOP_TIME_PS
    assert stop.instructions == STOP_INSTRUCTIONS


@pytest.mark.parametrize("budget", [1e-9, 60.0])
def test_a_python_stop_when_stops_at_the_same_point_at_any_wall_budget(
    u575: popcorn.PartDescription, budget: float
) -> None:
    m = member(u575, "tx_irq", "a", slice_wall_budget=budget)
    stop = asyncio.run(m.run(stop_when=tx_next_ge_3))
    assert stop.reason is StopReason.USER
    assert stop.time.ps == STOP_TIME_PS


def test_a_python_predicate_is_asked_as_often_at_any_wall_budget(
    u575: popcorn.PartDescription,
) -> None:
    asked: list[int] = []

    def never(m: popcorn.Machine) -> bool:
        asked.append(1)
        return False

    m = member(u575, "tx_irq", "a", slice_wall_budget=1e-9)
    stop = asyncio.run(m.run(for_=popcorn.units.us(300), stop_when=never))
    assert stop.reason is StopReason.TIME
    wall_ended = len(asked)
    quiet = member(u575, "tx_irq", "a", slice_wall_budget=60.0)
    asked.clear()
    asyncio.run(quiet.run(for_=popcorn.units.us(300), stop_when=never))
    assert 0 < wall_ended == len(asked)
