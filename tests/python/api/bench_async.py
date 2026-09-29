"""NFR-ASYNC-1: event loop tick gaps during a long run stay within 2x the slice wall budget.

The run is bounded by an instruction budget of 1e8 rather than the spec's `run(for_=s(10))`:
the ALU firmware never idles, so both run for a fixed stretch of work and the gaps measure the
same slices.

Opt-in, `POPCORN_BENCH=1`, on a quiet host.
"""

from __future__ import annotations

import asyncio
import os
import statistics
import time

import pytest

from .conftest import Machines

pytestmark = pytest.mark.skipif(
    os.environ.get("POPCORN_BENCH") != "1", reason="set POPCORN_BENCH=1 on a quiet host"
)

RUN = 100_000_000


async def gaps_during(machines: Machines, budget: float) -> list[float]:
    """Wall gaps between ticks of a task that yields as often as the loop lets it."""
    m = machines(slice_wall_budget=budget)
    gaps: list[float] = []
    done = False

    async def ticker() -> None:
        last = time.perf_counter()
        while not done:
            await asyncio.sleep(0)
            now = time.perf_counter()
            gaps.append(now - last)
            last = now

    task = asyncio.create_task(ticker())
    await m.run(budget=RUN)
    done = True
    await task
    return gaps


@pytest.mark.parametrize("budget", [1e-3, 5e-3])
def test_loop_gaps_stay_within_twice_the_wall_budget(machines: Machines, budget: float) -> None:
    gaps = sorted(asyncio.run(gaps_during(machines, budget)))
    p50 = statistics.median(gaps)
    p99 = gaps[int(0.99 * (len(gaps) - 1))]
    worst = gaps[-1]
    print(
        f"\nNFR-ASYNC-1 budget {budget * 1e3:.0f} ms: {len(gaps)} ticks, "
        f"p50 {p50 * 1e3:.3f} ms, p99 {p99 * 1e3:.3f} ms, max {worst * 1e3:.3f} ms, "
        f"max/budget {worst / budget:.2f}"
    )
    assert worst <= 2 * budget
