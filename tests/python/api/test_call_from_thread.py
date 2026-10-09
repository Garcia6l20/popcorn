"""`Machine.call(from_thread=)`: a call waits for thread mode, or reports a handler it lands in."""

from __future__ import annotations

import asyncio
import contextlib
import json
import warnings
from pathlib import Path
from typing import cast

import popcorn
import pytest
from popcorn.units import ns

from .conftest import CALLEE_ELF, M33_JSON, run_bounded

ICSR = 0xE000_ED04
PENDSTSET = 1 << 26
AIRCR = 0xE000_ED0C
SCR = 0xE000_ED10
SLEEPONEXIT = 1 << 1
SYSTICK = 15


def in_systick(m: popcorn.Machine) -> int:
    """Run `m` from reset to the entry of SysTick_Handler and return `ticks` there."""
    stop = asyncio.run(m.run(until="SysTick_Handler", budget=1_000_000))
    assert stop.reason is popcorn.StopReason.ADDRESS
    assert m.reg.xpsr & 0x1FF == SYSTICK
    return m.mem.u32[m.sym("ticks")]


def ticks_now(
    m: popcorn.Machine, from_thread: bool | None, budget: int | None = None
) -> popcorn.CallResult:
    return asyncio.run(m.call("ticks_now", from_thread=from_thread, budget=budget))


def test_a_call_from_thread_waits_for_the_handler(callee: popcorn.Machine) -> None:
    t = in_systick(callee)
    result = ticks_now(callee, True)
    assert result.stop.reason is popcorn.StopReason.RETURNED
    assert result.value == t + 1
    assert result.ipsr == 0
    assert result.began


def test_a_call_at_once_runs_over_the_handler(callee: popcorn.Machine) -> None:
    t = in_systick(callee)
    start = callee.snapshot()
    with warnings.catch_warnings():
        warnings.simplefilter("error")
        silent = ticks_now(callee, False)
    assert (silent.value, silent.ipsr, silent.began) == (t, SYSTICK, True)
    callee.restore(start)
    with pytest.warns(popcorn.HandlerCallWarning, match="exception 15") as caught:
        warned = ticks_now(callee, None)
    assert len(caught) == 1
    assert (warned.value, warned.ipsr) == (t, SYSTICK)


def test_a_tail_chained_handler_runs_before_the_call(callee: popcorn.Machine) -> None:
    t = in_systick(callee)
    callee.mem.u32[ICSR] = PENDSTSET
    result = ticks_now(callee, True)
    assert (result.value, result.ipsr) == (t + 2, 0)


def test_a_handler_that_never_returns_ends_the_call_unbegun(callee: popcorn.Machine) -> None:
    in_systick(callee)
    callee.reg.pc = callee.sym("forever")
    sp = callee.reg.sp
    result = ticks_now(callee, True, budget=10_000)
    assert result.stop.reason is popcorn.StopReason.BUDGET
    assert result.stop.detail == "before thread mode"
    assert not result.began
    assert result.ipsr == SYSTICK
    forever = callee.sym("forever")
    assert forever <= callee.pc < forever + 0x20
    assert callee.reg.sp == sp


def test_a_thread_ready_machine_calls_the_same_either_way(callee: popcorn.Machine) -> None:
    stop = asyncio.run(callee.run(budget=5_000))
    assert stop.reason is popcorn.StopReason.BUDGET
    assert callee.reg.xpsr & 0x1FF == 0
    start = callee.snapshot()
    results: list[tuple[popcorn.CallResult, int, int]] = []
    for from_thread in (True, False):
        callee.restore(start)
        result = ticks_now(callee, from_thread)
        results.append((result, callee.instructions, callee.time.ps))
    assert results[0] == results[1]


def test_the_wait_ends_where_single_steps_find_thread_mode(callee: popcorn.Machine) -> None:
    in_systick(callee)
    start = callee.snapshot()
    while True:
        asyncio.run(callee.step())
        vectpending = (callee.mem.u32[ICSR] >> 12) & 0x1FF
        if callee.reg.xpsr & 0x1FF == 0 and vectpending == 0:
            break
    expected = callee.pc
    callee.restore(start)
    assert ticks_now(callee, True).began
    assert callee.pc == expected


def test_a_fault_in_the_handler_ends_the_call_unbegun(callee: popcorn.Machine) -> None:
    in_systick(callee)
    callee.reg.pc = callee.sym("fault")
    result = ticks_now(callee, True, budget=10_000)
    assert result.stop.reason is popcorn.StopReason.FAULT
    assert not result.began
    assert result.stop.registers is None


def test_a_reset_request_in_the_handler_ends_the_call_unbegun(callee: popcorn.Machine) -> None:
    in_systick(callee)

    def reset(call: popcorn.Call) -> popcorn.Return:
        callee.mem.u32[AIRCR] = 0x05FA_0004
        return call.proceed()

    callee.intercept("tick", reset)
    result = ticks_now(callee, True, budget=10_000)
    assert result.stop.reason is popcorn.StopReason.RESET_REQUEST
    assert not result.began


def test_a_stop_in_a_tail_chained_handler_then_a_second_call_completes(
    callee: popcorn.Machine,
) -> None:
    t = in_systick(callee)
    callee.mem.u32[ICSR] = PENDSTSET
    entered: list[int] = []

    def stop_second(call: popcorn.Call) -> popcorn.Return:
        entered.append(1)
        return call.stop() if len(entered) == 2 else call.proceed()

    callee.intercept("tick", stop_second)
    first = ticks_now(callee, True)
    assert first.stop.reason is popcorn.StopReason.USER
    assert (first.began, first.ipsr) == (False, SYSTICK)
    second = ticks_now(callee, True)
    assert (second.began, second.ipsr, second.value) == (True, 0, t + 2)


def test_sleep_on_exit_counts_as_thread_mode(callee: popcorn.Machine) -> None:
    t = in_systick(callee)
    callee.mem.u32[SCR] = callee.mem.u32[SCR] | SLEEPONEXIT
    result = ticks_now(callee, True)
    assert (result.began, result.ipsr, result.value) == (True, 0, t + 1)


def test_a_cancel_during_the_wait_parks_the_machine_inside_the_handler(
    callee: popcorn.Machine,
) -> None:
    in_systick(callee)
    callee.reg.pc = callee.sym("forever")

    async def scenario() -> None:
        calling = asyncio.create_task(callee.call("ticks_now", from_thread=True, budget=None))
        await asyncio.sleep(0.05)
        calling.cancel()
        with contextlib.suppress(asyncio.CancelledError):
            await calling

    callee.default_budget = None
    run_bounded(scenario())
    forever = callee.sym("forever")
    assert forever <= callee.pc < forever + 0x20
    assert callee.reg.xpsr & 0x1FF == SYSTICK
    assert asyncio.run(callee.run(budget=100)).reason is popcorn.StopReason.BUDGET


def test_an_abi_the_core_lacks_raises_before_the_wait_runs(tmp_path: Path) -> None:
    description = json.loads(M33_JSON.read_text())
    description["core"]["fpu"] = False
    path = tmp_path / "m33_nofpu.json"
    path.write_text(json.dumps(description))
    m = popcorn.Machine(popcorn.load_description(path), image=popcorn.Image.elf(CALLEE_ELF))
    m.reset()
    asyncio.run(m.run(until="SysTick_Handler", budget=1_000_000))
    time, pc = m.time, m.pc
    with pytest.raises(popcorn.ConfigError):
        asyncio.run(m.call("ticks_now", from_thread=True, abi=popcorn.FloatAbi.HARD_DOUBLE))
    assert (m.time, m.pc) == (time, pc)


def test_a_systick_pending_at_a_time_stop_runs_before_a_call_from_thread(
    callee: popcorn.Machine,
) -> None:
    asyncio.run(callee.run(budget=5_000))
    start = callee.snapshot()
    for step in range(1, 400):
        callee.restore(start)
        asyncio.run(callee.run(for_=ns(100 * step)))
        if callee.mem.u32[ICSR] & PENDSTSET and callee.reg.xpsr & 0x1FF == 0:
            break
    else:
        pytest.fail("no TIME stop with SysTick pending")
    stopped = callee.snapshot()
    t = callee.mem.u32[callee.sym("ticks")]
    assert ticks_now(callee, False).value == t
    callee.restore(stopped)
    assert ticks_now(callee, True).value == t + 1


@pytest.mark.parametrize("given", ["no", 0, 1.0])
def test_from_thread_is_a_bool_or_none(callee: popcorn.Machine, given: object) -> None:
    with pytest.raises(TypeError, match="from_thread"):
        asyncio.run(callee.call("ticks_now", from_thread=cast(bool, given)))
    assert callee.instructions == 0
