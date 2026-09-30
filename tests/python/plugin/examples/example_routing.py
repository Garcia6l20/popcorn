"""Spec 03 section 10 in the shape a suite uses it: a blocking host call runs off the event loop
while the firmware runs.

The host call here waits on a queue the test fills. A real host library would talk to the firmware
over `machine.board.pty(...)`.
"""

from __future__ import annotations

import asyncio
import queue

import popcorn
import pytest
from popcorn.units import ms


def host_request(requests: queue.Queue[int]) -> int:
    """A blocking host call: wait for a request, answer it."""
    return requests.get(timeout=5) * 2


async def test_the_host_answers_while_the_firmware_runs(machine: popcorn.Machine) -> None:
    requests: queue.Queue[int] = queue.Queue()
    async with machine.running():
        answer = asyncio.create_task(asyncio.to_thread(host_request, requests))
        requests.put(21)
        assert await answer == 42
    stop = await machine.run(for_=ms(1))
    assert stop.reason is popcorn.StopReason.TIME
    assert machine.mem.u32[machine.sym("alu_rounds")] == 1


@pytest.mark.popcorn(budget=10_000)
async def test_a_budget_bounds_a_run(machine: popcorn.Machine) -> None:
    stop = await machine.run()
    assert stop.reason is popcorn.StopReason.BUDGET


async def test_calls_on_the_boot_state(machine: popcorn.Machine) -> None:
    await machine.call("alu_mark")
    assert machine.mem.u32[machine.sym("alu_rounds")] == 1
