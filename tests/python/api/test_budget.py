"""What `budget=` counts: executed instructions, not the time the core sleeps."""

from __future__ import annotations

import asyncio
import inspect

import popcorn
import popcorn.pytest
import pytest
from popcorn.pytest._options import Settings
from popcorn.units import ms

from .conftest import Machines

SLEEP_RULE = "not the time it sleeps in WFI or WFE"

DOCUMENTED = [
    popcorn.Machine.run,
    popcorn.Machine.call,
    popcorn.Machine.step_return,
    popcorn.Machine.default_budget,
    popcorn.Boot,
    popcorn.pytest,
    Settings,
]


@pytest.mark.parametrize("entity", DOCUMENTED, ids=lambda entity: getattr(entity, "__name__", "x"))
def test_every_budget_docstring_says_that_sleep_is_not_counted(entity: object) -> None:
    text = " ".join((inspect.getdoc(entity) or "").split())
    assert SLEEP_RULE in text


def test_the_budget_exceeded_budget_names_the_stop_count_sleep_included() -> None:
    text = " ".join((inspect.getdoc(popcorn.BudgetExceeded) or "").split())
    assert "since reset, sleep included" in text


def test_budget_counts_executed_instructions_not_sleep(machines: Machines) -> None:
    m = machines("idle")
    budget = 50
    stop = asyncio.run(m.run(budget=budget, for_=ms(1)))
    assert stop.reason is popcorn.StopReason.TIME
    assert m.executed <= budget
    assert stop.instructions - m.executed > 10 * budget
