"""Running a machine: every stop reason, step, wait_for, stop_when."""

from __future__ import annotations

import asyncio
import traceback

import popcorn
import pytest
from popcorn.units import VirtualTime, ms

from .conftest import Machines


def test_until_a_symbol_stops_exactly_before_it_with_its_source_line(
    machines: Machines, driver: popcorn.Image
) -> None:
    m = machines()
    stop = asyncio.run(m.run(until="alu_mark"))
    assert stop.reason is popcorn.StopReason.ADDRESS
    assert stop.pc == driver.lookup("alu_mark").value
    assert stop.location.symbol == "alu_mark"
    assert stop.location.file.endswith("/driver.c")
    assert stop.location.line == driver.locate(stop.pc).line > 0
    assert stop.instructions == m.instructions


def test_budget_stops_within_one_block_past_it(machines: Machines) -> None:
    m = machines(slice_instructions=100_000)
    stop = asyncio.run(m.run(budget=1_000_000))
    assert stop.reason is popcorn.StopReason.BUDGET
    assert 1_000_000 <= stop.instructions < 1_000_000 + 64


def test_for_stops_on_virtual_time(machines: Machines) -> None:
    m = machines()
    stop = asyncio.run(m.run(for_=ms(1)))
    assert stop.reason is popcorn.StopReason.TIME
    assert stop.time >= VirtualTime(0) + ms(1)
    assert m.time == stop.time


def test_for_counts_from_now_not_from_reset(machines: Machines) -> None:
    m = machines()
    asyncio.run(m.run(for_=ms(2)))
    before = m.time
    stop = asyncio.run(m.run(for_=ms(1)))
    assert stop.reason is popcorn.StopReason.TIME
    assert stop.time >= before + ms(1)


def test_wfi_with_nothing_pending_is_idle(machines: Machines) -> None:
    stop = asyncio.run(machines("idle").run())
    assert stop.reason is popcorn.StopReason.IDLE
    assert stop.location.symbol == "idle_main"


def test_fault_fills_the_fault(machines: Machines) -> None:
    stop = asyncio.run(machines("fault").run())
    assert stop.reason is popcorn.StopReason.FAULT
    assert stop.fault is not None
    assert stop.fault.kind is popcorn.FaultKind.BUS
    assert stop.fault.address == 0x6000_0000
    assert stop.location.symbol == "fault_main"


def test_sysresetreq_is_a_reset_request(machines: Machines) -> None:
    stop = asyncio.run(machines("reset").run())
    assert stop.reason is popcorn.StopReason.RESET_REQUEST
    assert stop.fault is None


def test_bkpt_is_a_user_stop(machines: Machines) -> None:
    stop = asyncio.run(machines("bkpt").run())
    assert stop.reason is popcorn.StopReason.USER
    assert stop.location.symbol == "bkpt_main"


def test_a_loop_making_no_progress_is_a_spin_naming_it(machines: Machines) -> None:
    stop = asyncio.run(machines("spin", spin_threshold=1000).run())
    assert stop.reason is popcorn.StopReason.SPIN
    assert "spin_main" in stop.detail


def test_stop_when_stops_with_user_and_may_read_the_machine(machines: Machines) -> None:
    m = machines(slice_instructions=10_000)
    stop = asyncio.run(m.run(stop_when=lambda machine: machine.instructions >= 30_000))
    assert stop.reason is popcorn.StopReason.USER
    assert 30_000 <= stop.instructions < 30_000 + 10_000 + 64


def test_step_executes_exactly_n_instructions(machines: Machines) -> None:
    m = machines()
    asyncio.run(m.step(3))
    before = m.instructions
    stop = asyncio.run(m.step(10))
    assert stop.reason is popcorn.StopReason.BUDGET
    assert stop.instructions == before + 10


def test_wait_for_reached_stops_exactly(machines: Machines, driver: popcorn.Image) -> None:
    m = machines()
    stop = asyncio.run(m.wait_for(popcorn.Reached("alu_mark")))
    assert stop.reason is popcorn.StopReason.ADDRESS
    assert stop.pc == driver.lookup("alu_mark").value


class AfterInstructions:
    """Ready once the machine has executed a count of instructions."""

    def __init__(self, count: int) -> None:
        self.count = count

    def ready(self, machine: popcorn.Machine) -> bool:
        """Whether the count is reached."""
        return machine.instructions >= self.count


def test_wait_for_an_event_stops_within_one_slice_of_it(machines: Machines) -> None:
    m = machines(slice_instructions=10_000)
    stop = asyncio.run(m.wait_for(AfterInstructions(25_000)))
    assert stop.reason is popcorn.StopReason.USER
    assert 25_000 <= stop.instructions < 25_000 + 10_000 + 64


def test_wait_for_ends_on_its_virtual_deadline(machines: Machines) -> None:
    m = machines()
    stop = asyncio.run(m.wait_for(AfterInstructions(10**12), for_=ms(2)))
    assert stop.reason is popcorn.StopReason.TIME


def failing_predicate(machine: popcorn.Machine) -> bool:
    """A stop_when that fails once some instructions ran."""
    if machine.instructions > 100:
        raise LookupError("predicate failed")
    return False


def test_a_stop_when_error_reaches_the_caller_with_its_traceback(machines: Machines) -> None:
    m = machines(slice_instructions=1_000)
    with pytest.raises(LookupError, match="predicate failed") as caught:
        asyncio.run(m.run(stop_when=failing_predicate, budget=100_000))
    frames = [frame.name for frame in traceback.extract_tb(caught.value.__traceback__)]
    assert "failing_predicate" in frames
    assert not m.busy


def test_a_stop_when_error_leaves_the_machine_resumable(machines: Machines) -> None:
    m = machines(slice_instructions=1_000)
    with pytest.raises(LookupError):
        asyncio.run(m.run(stop_when=failing_predicate))
    before = m.instructions
    stop = asyncio.run(m.run(budget=1000))
    assert stop.reason is popcorn.StopReason.BUDGET
    assert stop.instructions >= before + 1000


def test_unknown_until_symbol_is_a_symbol_error(machines: Machines) -> None:
    with pytest.raises(popcorn.SymbolError):
        asyncio.run(machines().run(until="alu_mrk"))


def test_spec_section_2_example_types_and_runs(machines: Machines) -> None:
    from popcorn.units import MHz, us

    async def example(m: popcorn.Machine) -> popcorn.Stop:
        stop = await m.run(until=m.address_of("alu_mark"), budget=500_000_000)
        assert stop.reason is popcorn.StopReason.ADDRESS
        return await m.run(for_=us(10))

    stop = asyncio.run(example(machines()))
    assert stop.reason is popcorn.StopReason.TIME
    assert MHz(4).period == us(1) // 4


def test_a_negative_for_is_a_value_error(machines: Machines) -> None:
    m = machines()
    with pytest.raises(ValueError, match="negative"):
        asyncio.run(m.run(for_=ms(-1)))
    with pytest.raises(ValueError, match="negative"):
        asyncio.run(m.wait_for(AfterInstructions(1), for_=ms(-1)))
    assert not m.busy


@pytest.mark.xfail(strict=True, reason="run: until at the start pc stops after 0 instructions")
def test_a_run_from_its_until_address_goes_on_to_the_next_hit(hooked: popcorn.Machine) -> None:
    hooked.reg.pc = hooked.sym("write_three")
    arguments: list[int] = []
    for _ in range(3):
        before = hooked.instructions
        stop = asyncio.run(hooked.run(until="writer", budget=10_000))
        assert stop.reason is popcorn.StopReason.ADDRESS
        assert stop.instructions > before
        arguments.append(hooked.reg.r0)
    assert arguments == [1, 2, 3]
