"""FR-CALL-6: per-call reset cost of `call(reset=True)` on a leaf function.

Opt-in, `POPCORN_BENCH=1`, on a quiet host.
"""

from __future__ import annotations

import asyncio
import os
import statistics
import time

import popcorn
import pytest

pytestmark = pytest.mark.skipif(
    os.environ.get("POPCORN_BENCH") != "1", reason="set POPCORN_BENCH=1 on a quiet host"
)

CALLS = 10_000


@pytest.mark.xfail(strict=True, reason="FR-CALL-6: 25 us median not met in v1, gap accepted (Q10)")
def test_reset_calls_meet_the_25_us_bound(callee: popcorn.Machine) -> None:
    writable = sum(r.size for r in callee.description.regions if r.acc.write) + (callee.arena.size)

    async def body() -> list[float]:
        await callee.call("add", 1, 2, reset=True)
        times: list[float] = []
        for index in range(CALLS):
            started = time.perf_counter()
            result = await callee.call("add", index, 1, reset=True)
            times.append(time.perf_counter() - started)
            assert result.value == index + 1
        return times

    times = sorted(asyncio.run(body()))
    median = statistics.median(times)
    print(
        f"\nFR-CALL-6: {CALLS} calls, {writable / 1024:.0f} KiB writable, median "
        f"{median * 1e6:.1f} us, p99 {times[int(0.99 * (CALLS - 1))] * 1e6:.1f} us, "
        f"{median * 1e6 / (writable / (1 << 20)):.1f} us per MiB"
    )
    assert median <= 25e-6
