"""The IWDG example refreshing past the profile write log cap."""

from __future__ import annotations

import asyncio
import inspect

import popcorn
from popcorn.profiling._watchdog import watchdog_registers
from popcorn.units import ms

from .test_profiling import booted
from .test_profiling_extras import IWDG

BURST = 5000
INIT_KR_WRITES = 3


def test_a_refresh_burst_past_the_log_cap_drops_the_rest_and_counts_them() -> None:
    """`HAL_IWDG_Init` ends with three KR stores: the 0xCCCC start, the 0x5555 unlock and, with
    the window disabled, the 0xAAAA reload."""
    m = booted(IWDG)
    profile_fn = popcorn.Machine._profile  # pyright: ignore[reportPrivateUsage]
    limit = inspect.signature(profile_fn).parameters["write_log_limit"].default
    dog = watchdog_registers(m.description, lambda at: int.from_bytes(m.mem.read(at, 4), "little"))
    assert dog is not None
    refreshes = [0]

    def count(call: popcorn.Call) -> popcorn.Return:
        refreshes[0] += 1
        return call.proceed()

    m.intercept("HAL_IWDG_Refresh", count)
    with m._profile(  # pyright: ignore[reportPrivateUsage]
        logged_registers=dog.logged, write_log_limit=limit, write_clock_domain="lsi"
    ) as recording:
        m.var("refresh_burst").value = BURST
        for _ in range(200):
            asyncio.run(m.run(for_=ms(5)))
            if m.var("refresh_burst").value == 0:
                break
        assert m.var("refresh_burst").value == 0
    log = next(log for log in recording.result.write_logs if log.id in dog.kr)
    assert len(log.writes) == limit == 4096
    assert refreshes[0] >= BURST
    assert log.dropped >= BURST - limit
    assert len(log.writes) + log.dropped == refreshes[0] + INIT_KR_WRITES
