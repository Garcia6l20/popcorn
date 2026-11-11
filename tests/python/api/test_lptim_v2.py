"""The STM32U5 LPTIM: every instance has a model, and the LL firmware's matches come at the same
virtual times parked and running."""

from __future__ import annotations

import asyncio
from collections.abc import Callable
from pathlib import Path

import popcorn
import pytest
from popcorn.stm32 import mcu, support
from popcorn.units import ms, us

from .conftest import run_bounded

PREBUILT = Path(__file__).parents[2] / "firmware" / "periph" / "prebuilt" / "lptim_ll.elf"
U5_PARTS = ("STM32U575QGIx", "STM32U585AIIx")
INSTANCES = ("LPTIM1", "LPTIM2", "LPTIM3", "LPTIM4")
PERIODS = 10


@pytest.mark.parametrize("part", U5_PARTS)
def test_every_u5_lptim_has_a_builtin_model(part: str) -> None:
    models = {p.name: p.model for p in support(mcu(part))}
    assert [models[name] for name in INSTANCES] == ["builtin"] * len(INSTANCES)


def matches_of(m: popcorn.Machine) -> list[tuple[str, int]]:
    seen: list[tuple[str, int]] = []

    def recorder(name: str) -> Callable[[popcorn.Call], popcorn.Return]:
        def record(call: popcorn.Call) -> popcorn.Return:
            seen.append((name, m.time.ps))
            return call.proceed()

        return record

    m.intercept("on_compare", recorder("compare"))
    m.intercept("on_reload", recorder("reload"))
    return seen


def booted() -> popcorn.Machine:
    m = popcorn.MachineDescription(part="STM32U575QGIx", images=[popcorn.Elf(PREBUILT)]).machine()
    m.reset()
    m.reg.pc = m.sym("lptim1_forever")
    return m


def parked(m: popcorn.Machine) -> popcorn.Stop:
    return asyncio.run(m.run(for_=ms(5)))


def running(m: popcorn.Machine) -> popcorn.Stop:
    async def body() -> popcorn.Stop:
        async with m.running():
            return await m.wait_for(m.ref.var("reloads").ge(PERIODS))

    return run_bounded(body(), 20)


def test_the_matches_are_equal_parked_and_running() -> None:
    results: list[list[tuple[str, int]]] = []
    for run in (parked, running):
        m = booted()
        seen = matches_of(m)
        stop = run(m)
        assert stop.reason in (popcorn.StopReason.TIME, popcorn.StopReason.USER), stop
        assert m.var("reloads").value >= PERIODS
        results.append(seen[: 2 * PERIODS])
    assert results[0] == results[1]
    assert [name for name, _ in results[0]] == ["compare", "reload"] * PERIODS


async def test_ue_sets_at_every_third_overflow_with_rep_2() -> None:
    m = booted()
    m.reg.pc = m.sym("idle")
    m.periph.RCC.APB3ENR.LPTIM1EN = 1
    timer = m.periph.LPTIM1
    timer.CR.write(1)
    timer.RCR.write(2)
    timer.ARR.write(9)
    await m.run(for_=us(5))
    timer.CR.write(1 | 1 << 2)
    await m.run(for_=us(8))
    assert timer.ISR_output.UE == 0
    await m.run(for_=us(0.5))
    assert timer.ISR_output.UE == 1
    assert timer.ISR_output.REPOK == 1
