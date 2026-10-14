"""`Machine.probe`: native arrival counts, `hit(within=)` parked and in a free run."""

from __future__ import annotations

import _thread
import asyncio
import gc
import signal
import threading
import time
import weakref
from typing import cast

import popcorn
import pytest
from popcorn import _core
from popcorn._host import awaited
from popcorn._stop import stop_of
from popcorn.units import VirtualDuration, VirtualTime, ms, us

from .conftest import Machines, run_bounded

HANG = 30.0


def reached(machines: Machines, target: int | str, scenario: str | None = None) -> VirtualTime:
    """The time a parked `run(until=target)` stops at, on a fresh machine."""
    m = machines(scenario)
    if scenario == "jump":
        m.mem.u32[m.sym("jump_flag")] = 1
    stop = asyncio.run(m.run(until=target, budget=10_000_000))
    assert stop.reason is popcorn.StopReason.ADDRESS
    return stop.time


def test_a_probe_counts_a_jump_made_once_while_the_machine_free_runs(machines: Machines) -> None:
    m = machines("jump")
    flag = m.ref.var("jump_flag")

    async def scenario() -> tuple[bool, int, VirtualTime | None, bool, int]:
        async with m.running():
            boot = m.probe("app_entry")
            ticks = m.probe("tick")
            assert boot.count == 0
            await m.write((flag, 1))
            hit = await boot.hit(within=ms(2000))
            assert await ticks.hit(within=ms(2000))
            again = await boot.hit(within=VirtualDuration(0))
            return hit, boot.count, boot.first_at, again, ticks.count

    hit, count, first_at, again, ticks = run_bounded(scenario(), HANG)
    assert hit and again
    assert count == 1
    assert isinstance(first_at, VirtualTime)
    assert ticks > 0


def test_a_hit_answers_the_same_parked_and_running(machines: Machines) -> None:
    mark = reached(machines, "alu_mark")
    parked = machines()
    probe = parked.probe("alu_mark")
    assert asyncio.run(probe.hit(within=ms(2)))
    assert (probe.count, probe.first_at) == (1, mark)
    assert parked.time > mark

    running = machines()
    probe = running.probe("alu_mark")
    missing = running.probe("idle_main")

    async def scenario() -> tuple[bool, bool]:
        async with running.running():
            return await probe.hit(within=ms(2)), await missing.hit(within=ms(1))

    assert run_bounded(scenario(), HANG) == (True, False)
    assert (probe.count, probe.first_at) == (1, mark)
    assert (missing.count, missing.first_at) == (0, None)


def test_a_hit_already_counted_returns_true_without_running(machines: Machines) -> None:
    m = machines()
    probe = m.probe("alu_mark")
    asyncio.run(m.run(until="idle_main", for_=ms(10)))
    assert probe.count == 1
    before = m.time
    assert asyncio.run(probe.hit(within=VirtualDuration(0)))
    assert m.time == before


def test_a_miss_lets_the_time_pass_and_answers_false(machines: Machines) -> None:
    m = machines()
    probe = m.probe("idle_main")
    start = m.time
    assert not asyncio.run(probe.hit(within=ms(5)))
    assert m.time >= start + ms(5)
    assert m.time < start + ms(5) + us(1)
    assert (probe.count, probe.first_at) == (0, None)
    assert not asyncio.run(probe.hit(within=VirtualDuration(0)))


def test_the_end_of_within_is_exclusive_wherever_blocks_fall(machines: Machines) -> None:
    for target in ("alu_mark", "alu_mark+2"):
        m0 = machines()
        address = m0.sym("alu_mark") + (2 if target.endswith("+2") else 0)
        at = reached(machines, address)
        for extra, want in ((0, False), (1, True)):
            m = machines()
            probe = m.probe(address)
            within = VirtualDuration(at.ps - m.time.ps + extra)
            assert asyncio.run(probe.hit(within=within)) is want, (target, extra)
            if target.endswith("+2"):
                assert probe.first_at == at


def test_count_and_first_at_follow_one_three_and_zero_arrivals(machines: Machines) -> None:
    m = machines("jump")
    m.mem.u32[m.sym("jump_flag")] = 1
    boot, ticks, never = m.probe("app_entry"), m.probe("tick"), m.probe("idle_main")
    first_tick = reached(machines, "tick", "jump")
    asyncio.run(m.run(until="tick", budget=1_000_000))
    asyncio.run(m.step())
    for _ in range(2):
        asyncio.run(m.run(until="tick", budget=1_000_000))
    asyncio.run(m.step())
    assert (boot.count, ticks.count, never.count) == (1, 3, 0)
    assert boot.first_at is not None and boot.first_at < first_tick
    assert ticks.first_at == first_tick
    assert never.first_at is None


def test_within_is_checked_at_the_call(machines: Machines) -> None:
    m = machines()
    probe = m.probe("idle_main")
    with pytest.raises(TypeError, match="VirtualDuration"):
        asyncio.run(probe.hit(within=VirtualTime(5)))  # pyright: ignore[reportArgumentType]
    with pytest.raises(ValueError, match="within must not be negative"):
        asyncio.run(probe.hit(within=VirtualDuration(-1)))
    assert m.time == VirtualTime(0)


def test_a_with_block_removes_the_probe_parked_and_running(machines: Machines) -> None:
    m = machines("jump")
    m.mem.u32[m.sym("jump_flag")] = 1
    with m.probe("tick") as ticks:
        asyncio.run(m.run(until="tick", budget=1_000_000))
        asyncio.run(m.step())
    assert not ticks.active
    asyncio.run(m.run(for_=us(50)))
    assert ticks.count == 1
    never = m.probe("idle_main")
    never.remove()
    before = m.time
    assert asyncio.run(ticks.hit(within=ms(1)))
    assert not asyncio.run(never.hit(within=ms(1)))
    assert m.time == before

    async def scenario() -> tuple[int, int]:
        async with m.running():
            with m.probe("tick") as inside:
                assert await inside.hit(within=ms(1))
            held = inside.count
            await m.elapse(us(100))
            return held, inside.count

    held, later = run_bounded(scenario(), HANG)
    assert held == later > 0


def test_remove_hooks_removes_probes_and_keeps_their_counts(machines: Machines) -> None:
    m = machines()
    probe = m.probe("alu_mark")
    m.intercept("idle_main", lambda c: c.proceed())
    asyncio.run(m.run(for_=ms(10)))
    assert m.remove_hooks() == 2
    assert not probe.active
    assert probe.count == 1
    assert asyncio.run(probe.hit(within=ms(1)))


def test_a_restore_keeps_the_probe_and_does_not_rewind_its_counts(machines: Machines) -> None:
    m = machines()
    saved = m.snapshot()
    probe = m.probe("alu_mark")
    asyncio.run(m.run(for_=ms(10)))
    first = probe.first_at
    m.restore(saved)
    assert probe.active
    asyncio.run(m.run(for_=ms(10)))
    assert probe.count == 2
    assert probe.first_at == first


def test_a_probe_shares_its_address_with_an_intercept(machines: Machines) -> None:
    m = machines()
    calls: list[int] = []
    before = m.probe("alu_mark")

    @m.intercept("alu_mark")
    def skip(c: popcorn.Call) -> popcorn.Return:
        calls.append(c.occurrence)
        return c.returns()

    after = m.probe("alu_mark")
    asyncio.run(m.run(for_=ms(10)))
    assert calls == [0]
    assert before.count == after.count == 1
    assert m.mem.u32[m.sym("alu_rounds")] == 0
    assert skip.active


def test_a_hit_raises_as_wait_until_on_another_stop(machines: Machines) -> None:
    m = machines("fault")
    with pytest.raises(popcorn.FaultError):
        asyncio.run(m.probe("idle_main").hit(within=ms(1)))

    m = machines()
    m.default_budget = 1_000
    with pytest.raises(popcorn.BudgetExceeded):
        asyncio.run(m.probe("idle_main").hit(within=ms(1)))

    m = machines()
    m.intercept("alu_mark", lambda c: c.stop())
    with pytest.raises(popcorn.EmulationError, match="USER"):
        asyncio.run(m.probe("idle_main").hit(within=ms(10)))

    m = machines("fault")

    async def scenario() -> None:
        async with m.running():
            await m.probe("idle_main").hit(within=ms(1000))

    with pytest.raises(popcorn.FaultError):
        run_bounded(scenario(), HANG)


def test_a_probe_from_a_handler_is_refused_and_from_a_posted_body_is_added(
    machines: Machines,
) -> None:
    m = machines()
    refused: list[BaseException] = []

    def handler(c: popcorn.Call) -> popcorn.Return:
        try:
            m.probe("idle_main")
        except popcorn.MachineRunning as error:
            refused.append(error)
        return c.proceed()

    m.intercept("alu_mark", handler)
    asyncio.run(m.run(for_=ms(10)))
    assert len(refused) == 1
    m.remove_hooks()

    async def scenario() -> popcorn.Probe:
        async with m.running():
            return await m.sample(lambda: m.probe("idle_main"))

    probe = run_bounded(scenario(), HANG)
    assert probe.active and probe.count == 0


def test_a_thousand_hits_take_no_gil(machines: Machines) -> None:
    m = machines("jump")
    m.mem.u32[m.sym("jump_flag")] = 1
    ticks = m.probe("tick")
    gc.collect()
    before = m._handle.gil_entries()  # pyright: ignore[reportPrivateUsage]
    while ticks.count < 1_000:
        asyncio.run(m.run(for_=us(20)))

    async def scenario() -> bool:
        async with m.running():
            return await m.probe("idle_main").hit(within=ms(1))

    assert not run_bounded(scenario(), HANG)
    gc.collect()
    assert m._handle.gil_entries() == before  # pyright: ignore[reportPrivateUsage]
    assert ticks.count >= 1_000


def test_probes_added_and_removed_a_thousand_times_leave_nothing(machines: Machines) -> None:
    m = machines()
    kept = weakref.ref(m.probe("alu_mark"))
    for _ in range(1_000):
        with m.probe("idle_main"):
            pass
    assert len(m._installed) <= 2  # pyright: ignore[reportPrivateUsage]
    assert m.remove_hooks() == 1
    gc.collect()
    assert kept() is None


def test_a_machine_with_a_probe_is_collected(machines: Machines) -> None:
    m = machines()
    probe = m.probe("alu_mark")
    asyncio.run(probe.hit(within=ms(2)))
    gone = weakref.ref(m)
    held = weakref.ref(probe)
    del m, probe
    gc.collect()
    assert gone() is None
    assert held() is None


def test_a_probe_counts_an_arrival_once_after_the_intercept_that_stopped_there_goes(
    machines: Machines,
) -> None:
    m = machines()
    probe = m.probe("alu_mark")
    stopper = m.intercept("alu_mark", lambda c: c.stop())
    assert asyncio.run(m.run(for_=ms(10))).reason is popcorn.StopReason.USER
    assert probe.count == 1
    stopper.remove()
    asyncio.run(m.step())
    assert probe.count == 1


def test_a_free_run_wait_ends_its_time_at_or_after_the_deadline_it_returns(
    machines: Machines,
) -> None:
    m = machines()
    handle = m._handle  # pyright: ignore[reportPrivateUsage]

    async def scenario() -> tuple[popcorn.Stop, bool, int | None]:
        async with m.running():
            never = handle.predicate_pc_at(0xFFFF_FFF0)
            raw, ended, deadline = cast(
                tuple[_core.StopInfo, bool, int | None],
                await awaited(handle.wait_predicate_timed(never, ms(1).ps), lambda got: got),
            )
            return stop_of(raw), ended, deadline

    stop, ended, deadline = run_bounded(scenario(), HANG)
    assert not ended
    assert stop.reason is popcorn.StopReason.TIME
    assert deadline is not None
    assert ms(1).ps <= deadline <= stop.time.ps


def test_ctrl_c_while_waiting_for_the_boundary_adds_nothing_and_still_removes(
    machines: Machines,
) -> None:
    m = machines("jump")
    m.mem.u32[m.sym("jump_flag")] = 1
    ticks = m.probe("tick")
    entered = threading.Event()
    released = threading.Event()

    def hold() -> None:
        entered.set()
        released.wait(HANG)

    def interrupt(waits: int) -> None:
        deadline = time.monotonic() + HANG
        while _core.interruptible_waits() == waits and time.monotonic() < deadline:
            time.sleep(0.001)
        _thread.interrupt_main()

    calls = [lambda: m.probe("idle_main"), ticks.remove]
    previous = signal.signal(signal.SIGINT, signal.default_int_handler)
    try:
        m.start()
        for call in calls:
            entered.clear()
            released.clear()
            held = m.sample(hold)
            assert entered.wait(HANG)
            waits = _core.interruptible_waits()
            threading.Thread(target=interrupt, args=(waits,), daemon=True).start()
            with pytest.raises(KeyboardInterrupt):
                call()
            assert _core.interruptible_waits() == waits + 1
            released.set()
            asyncio.run(held)
        assert not ticks.active
        m.halt()
    finally:
        released.set()
        signal.signal(signal.SIGINT, previous)
    counted = ticks.count
    assert counted > 0
    asyncio.run(m.run(for_=us(200)))
    assert ticks.count == counted
    assert m.remove_hooks() == 0


def test_a_hit_during_a_run_awaited_elsewhere_is_refused_as_busy(machines: Machines) -> None:
    m = machines()
    probe = m.probe("idle_main")

    async def scenario() -> None:
        running = asyncio.create_task(m.run(for_=ms(50)))
        await asyncio.sleep(0)
        with pytest.raises(popcorn.MachineBusy):
            await probe.hit(within=ms(1))
        await running

    run_bounded(scenario(), HANG)
