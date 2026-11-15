"""`Machine(on_reset_request=...)`: a firmware reset request stops the run or is applied and the
run goes on, and `Machine.resets` counts the resets."""

from __future__ import annotations

import asyncio
from typing import Literal, cast

import popcorn
import pytest

from .conftest import Machines

RESET = popcorn.StopReason.RESET_REQUEST
BUDGET = popcorn.StopReason.BUDGET
LOOP_BUDGET = 2_000
INSTRUCTIONS_PER_REQUEST = 4


def requests_in(machines: Machines, budget: int) -> int:
    """Firmware reset requests a stop-mode run makes before `budget` instructions, resuming
    after each."""
    m = machines("reset")
    requested = 0
    spent = 0
    while spent < budget:
        stop = asyncio.run(m.run(budget=budget - spent))
        if stop.reason is not RESET:
            break
        requested += 1
        spent += stop.instructions
    return requested


def test_a_reset_policy_resets_and_goes_on_through_a_loop_of_requests(
    machines: Machines,
) -> None:
    expected = requests_in(machines, LOOP_BUDGET)
    assert expected == LOOP_BUDGET // INSTRUCTIONS_PER_REQUEST
    m = machines("reset", on_reset_request="reset")
    before = m.resets
    stop = asyncio.run(m.run(budget=LOOP_BUDGET))
    assert stop.reason is BUDGET
    assert m.resets - before == expected
    assert stop.resets == m.resets


@pytest.mark.parametrize("explicit", [False, True], ids=["default", "explicit"])
def test_the_stop_policy_stops_at_the_first_request_and_the_next_run_applies_it(
    machines: Machines, explicit: bool
) -> None:
    m = machines("reset", on_reset_request="stop") if explicit else machines("reset")
    before = m.resets
    stop = asyncio.run(m.run(budget=LOOP_BUDGET))
    assert stop.reason is RESET
    assert stop.detail == "software"
    assert stop.time.ps > 0
    assert m.resets == before
    asyncio.run(m.run(budget=1))
    assert m.resets == before + 1


def test_a_call_under_the_reset_policy_stops_at_the_request(machines: Machines) -> None:
    m = machines(on_reset_request="reset")
    asyncio.run(m.run(budget=10_000))
    before = m.resets
    result = asyncio.run(m.call("reset_main", budget=1_000))
    assert result.stop.reason is RESET
    assert m.resets == before


@pytest.mark.parametrize("value", ["halt", "", None])
def test_an_unknown_reset_policy_raises_before_a_machine_is_built(
    m33: popcorn.PartDescription, value: object
) -> None:
    with pytest.raises(ValueError, match=r'on_reset_request is "stop" or "reset"'):
        popcorn.Machine(m33, on_reset_request=cast(Literal["stop", "reset"], value))


@pytest.mark.parametrize("taken", ["stop", "reset"])
def test_a_snapshot_restores_into_a_machine_of_the_other_policy(
    machines: Machines, taken: Literal["stop", "reset"]
) -> None:
    other: Literal["stop", "reset"] = "reset" if taken == "stop" else "stop"
    source = machines("reset", on_reset_request=taken)
    asyncio.run(source.run(budget=1))
    saved = source.snapshot()
    target = machines("reset", on_reset_request=other)
    resets = target.resets
    target.restore(popcorn.Snapshot.from_bytes(saved.to_bytes()))
    assert target.resets == resets
    stop = asyncio.run(target.run(budget=LOOP_BUDGET))
    assert stop.reason is (BUDGET if other == "reset" else RESET)


AIRCR = 0xE000_ED0C
SYSRESETREQ = 0x05FA_0004


def test_a_free_run_and_a_budgeted_run_apply_the_same_request(machines: Machines) -> None:
    parked = machines(on_reset_request="reset")
    parked.mem.u32[AIRCR] = SYSRESETREQ
    before = parked.resets
    stop = asyncio.run(parked.run(until="alu_mark", budget=1_000_000))
    assert stop.reason is popcorn.StopReason.ADDRESS

    free = machines(on_reset_request="reset")
    free.mem.u32[AIRCR] = SYSRESETREQ

    async def run_free() -> popcorn.Stop:
        free.start()
        try:
            return await free.wait_for(popcorn.Reached("alu_mark"))
        finally:
            free.halt()

    reached = asyncio.run(run_free())
    assert reached.resets == stop.resets == before + 1
    assert free.resets == parked.resets


def test_a_reset_policy_machine_is_refused_as_a_system_member(
    m33: popcorn.PartDescription, driver: popcorn.Image
) -> None:
    kept = popcorn.Machine(m33, image=driver, name="a")
    refused = popcorn.Machine(m33, image=driver, name="b", on_reset_request="reset")
    with pytest.raises(popcorn.ConfigError, match=r"members\.b: .*on_member_reset"):
        popcorn.System({"a": kept, "b": refused}, [])


def test_a_machine_description_passes_the_reset_policy_to_its_machine() -> None:
    description = popcorn.MachineDescription(part="STM32U575QGIx", images=())
    kept = description.machine(name="a")
    refused = description.machine(name="b", on_reset_request="reset")
    with pytest.raises(popcorn.ConfigError, match=r"members\.b: .*on_member_reset"):
        popcorn.System({"a": kept, "b": refused}, [])
    with pytest.raises(ValueError, match=r'on_reset_request is "stop" or "reset"'):
        description.machine(on_reset_request=cast(Literal["stop", "reset"], "halt"))
