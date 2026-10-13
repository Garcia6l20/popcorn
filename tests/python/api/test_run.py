"""Running a machine: every stop reason, step, wait_for, stop_when."""

from __future__ import annotations

import asyncio
import traceback
from collections.abc import Awaitable, Callable

import popcorn
import pytest
from popcorn.units import MHz, VirtualDuration, VirtualTime, ms

from .conftest import CALLEE_ELF, Machines

M33_RESET_CLOCK = MHz(4)


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


def test_a_run_from_its_until_address_with_nothing_to_spend_stops_at_once(
    hooked: popcorn.Machine,
) -> None:
    hooked.reg.pc = hooked.sym("writer")
    before = hooked.instructions
    budget = asyncio.run(hooked.run(until="writer", budget=0))
    timed = asyncio.run(hooked.run(until="writer", for_=ms(0)))
    assert (budget.reason, timed.reason) == (popcorn.StopReason.BUDGET, popcorn.StopReason.TIME)
    assert hooked.instructions == before
    assert hooked.pc == hooked.sym("writer")


def test_elapse_parked_returns_what_run_for_returns(machines: Machines) -> None:
    m = machines()
    asyncio.run(m.run(for_=ms(1)))
    start = m.time
    snapshot = m.snapshot()
    elapsed = asyncio.run(m.elapse(ms(5)))
    elapsed_stop = m.last_stop
    m.restore(snapshot)
    ran = asyncio.run(m.run(for_=ms(5)))
    assert elapsed == ran.time == m.time
    assert elapsed_stop == ran
    assert start + ms(5) <= elapsed
    assert elapsed - (start + ms(5)) < M33_RESET_CLOCK.period * 512


def test_elapse_of_zero_executes_nothing(machines: Machines) -> None:
    m = machines()
    asyncio.run(m.run(for_=ms(1)))
    before = (m.time, m.instructions)
    assert asyncio.run(m.elapse(VirtualDuration(0))) == before[0]
    assert (m.time, m.instructions) == before


def test_elapse_parked_raises_a_fault_before_the_duration(machines: Machines) -> None:
    m = machines()
    start = m.time

    async def scenario() -> None:
        moved = m.write((m.ref.reg.pc, 0x3000_0000), at=start + ms(2))
        with pytest.raises(popcorn.FaultError):
            await m.elapse(ms(5))
        await moved

    asyncio.run(scenario())
    assert start + ms(2) <= m.time < start + ms(5)
    assert not m.busy


def test_elapse_parked_raises_on_its_budget(machines: Machines) -> None:
    m = machines()
    m.default_budget = 10
    with pytest.raises(popcorn.BudgetExceeded):
        asyncio.run(m.elapse(ms(5)))


def test_cancelling_a_parked_elapse_halts_the_run(machines: Machines) -> None:
    m = machines()

    async def scenario() -> None:
        elapsing = asyncio.create_task(m.elapse(ms(10_000)))
        await m.read(at=m.time + ms(1))
        elapsing.cancel()
        with pytest.raises(asyncio.CancelledError):
            await elapsing
        assert not m.busy

    asyncio.run(scenario())
    assert ms(1).ps <= m.time.ps < ms(10_000).ps


def test_a_negative_elapse_is_a_value_error_and_runs_nothing(machines: Machines) -> None:
    m = machines()
    with pytest.raises(ValueError, match="negative"):
        asyncio.run(m.elapse(VirtualDuration(-1)))
    assert m.instructions == 0
    assert not m.busy


def test_now_parked_is_the_time(machines: Machines) -> None:
    m = machines()
    asyncio.run(m.run(for_=ms(1)))
    assert asyncio.run(_now(m)) == m.time


async def _now(m: popcorn.Machine) -> VirtualTime:
    return await m.now()


@pytest.mark.parametrize("request_", ["elapse", "now"])
def test_elapse_and_now_from_a_handler_raise_runtime_error(
    machines: Machines, request_: str
) -> None:
    m = machines()

    def handler(_: object) -> None:
        if request_ == "now":
            m.now()
        else:
            asyncio.run(m.elapse(ms(1)))

    m.on_return("alu_mark", handler)
    with pytest.raises(RuntimeError, match="cannot await a machine request"):
        asyncio.run(m.run(for_=ms(10)))


def test_wait_until_returns_none_at_the_deadline(machines: Machines) -> None:
    m = machines()
    assert asyncio.run(m.wait_until(lambda _: False, for_=ms(2))) is None
    assert m.last_stop is not None
    assert m.last_stop.reason is popcorn.StopReason.TIME


def ticks_reach(count: int) -> Callable[[popcorn.Machine], bool]:
    """A predicate holding once the callee firmware's SysTick counted `count` ticks."""
    return lambda m: int(m.var("ticks").value) >= count


def test_wait_until_parked_returns_where_stop_when_stops(m33: popcorn.PartDescription) -> None:
    callee = popcorn.Machine(
        m33,
        image=popcorn.Image.elf(CALLEE_ELF),
        slice_instructions=10**9,
        slice_wall_budget=10.0,
    )
    callee.reset()
    snapshot = callee.snapshot()
    held = asyncio.run(callee.wait_until(ticks_reach(3), for_=ms(100)))
    callee.restore(snapshot)
    stop = asyncio.run(callee.run(stop_when=ticks_reach(3), for_=ms(100)))
    assert stop.reason is popcorn.StopReason.USER
    assert held == stop.time
    assert int(callee.var("ticks").value) == 3


def test_wait_until_parked_on_an_idle_core_returns_none(machines: Machines) -> None:
    m = machines("idle")
    assert asyncio.run(m.wait_until(lambda _: False)) is None
    assert m.last_stop is not None
    assert m.last_stop.reason is popcorn.StopReason.IDLE


def test_wait_until_raises_on_an_intercept_stop(machines: Machines) -> None:
    m = machines()
    m.intercept("alu_mark", lambda call: call.stop())
    with pytest.raises(popcorn.EmulationError, match="waiting for the predicate") as caught:
        asyncio.run(m.wait_until(lambda _: False, for_=ms(100)))
    assert caught.value.reason is popcorn.StopReason.USER


def test_wait_until_parked_gives_the_machine_and_holds_at_once(machines: Machines) -> None:
    m = machines()
    seen: list[popcorn.Machine] = []

    def same(machine: popcorn.Machine) -> bool:
        seen.append(machine)
        return machine is m

    assert asyncio.run(m.wait_until(same)) == m.time
    assert seen == [m]
    assert m.instructions == 0


def test_a_wait_until_error_reaches_the_caller_parked(machines: Machines) -> None:
    m = machines(slice_instructions=1_000)
    with pytest.raises(LookupError, match="predicate failed") as caught:
        asyncio.run(m.wait_until(failing_predicate, for_=ms(100)))
    frames = [frame.name for frame in traceback.extract_tb(caught.value.__traceback__)]
    assert "failing_predicate" in frames
    assert not m.busy


def test_wait_until_from_a_handler_raises_runtime_error(machines: Machines) -> None:
    m = machines()

    def handler(_: object) -> None:
        asyncio.run(m.wait_until(lambda _: True))

    m.on_return("alu_mark", handler)
    with pytest.raises(RuntimeError, match="cannot await a machine request"):
        asyncio.run(m.run(for_=ms(10)))


def test_wait_until_while_a_run_is_active_raises_machine_busy(machines: Machines) -> None:
    m = machines()
    calls: list[popcorn.Machine] = []

    def counted(machine: popcorn.Machine) -> bool:
        calls.append(machine)
        return True

    async def scenario() -> None:
        run = asyncio.create_task(m.run())
        await asyncio.sleep(0)
        with pytest.raises(popcorn.MachineBusy):
            await m.wait_until(counted)
        run.cancel()
        with pytest.raises(asyncio.CancelledError):
            await run

    asyncio.run(scenario())
    assert calls == []
    assert not m.busy


def test_the_parked_pre_check_of_wait_until_may_request(machines: Machines) -> None:
    m = machines()
    asyncio.run(m.run(for_=ms(1)))
    posted: list[Awaitable[VirtualTime]] = []

    def requesting(machine: popcorn.Machine) -> bool:
        posted.append(machine.now())
        return True

    async def scenario() -> tuple[VirtualTime | None, VirtualTime]:
        held = await m.wait_until(requesting)
        return held, await posted[0]

    held, requested = asyncio.run(scenario())
    assert held == requested == m.time
