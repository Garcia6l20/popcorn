"""LPTIM1 on the 32.768 kHz LSE in continuous PWM mode, HAL: an autoreload match every 125 ms and
a compare match half way through each period.

`HAL_LPTIM_MspInit` starts the LSE, selects it as the LPTIM1 kernel clock through
RCC_CCIPR.LPTIM1SEL 11, RM0351 6.4.28, enables LPTIM1 and its interrupt. The firmware starts the
counter with `HAL_LPTIM_PWM_Start_IT`, ARR 4095 and CMP 2047, so the counter wraps every 4096 LSE
cycles, 125 ms, and matches CMP 2048 cycles, 62.5 ms, before each wrap, RM0351 34.4.9, with ARRM and
CMPM in LPTIM_ISR, RM0351 34.7.1. The autoreload callback counts `periods` and toggles PA5, the
compare callback counts `compares`.

The boot stops at `lptim_started`, a few instructions after the CNTSTRT write. The counter starts
3 LSE cycles after the write, RM0351 34.4.6, reaches ARR 4095 cycles later, and ARRM sets one
cycle after that, ES0250 2.18.3. So the first autoreload callback runs 4099 LSE cycles after the
start, give or take the instructions on either side, and every later one exactly 125 ms after the
one before. The project sets
`MachineDescription(strict=True)`.
"""

import itertools
from collections.abc import Callable

import pytest
from popcorn import Call, Machine, Return, StopReason
from popcorn.units import VirtualTime, ms, s, us

pytestmark = pytest.mark.popcorn(budget=5_000_000)

PERIOD = ms(125)
PULSE_TO_RELOAD = ms(62.5)
LSE_HZ = 32_768
LSE_TICK = us(30.6)
FIRST_RELOAD_PS = (3 + 4095 + 1) * 10**12 // LSE_HZ
AROUND_CALLBACK = us(10)
LPTIM1SEL_LSE = 0b11


def matches(machine: Machine) -> list[tuple[str, VirtualTime]]:
    seen: list[tuple[str, VirtualTime]] = []

    def recorder(name: str) -> Callable[[Call], Return]:
        def record(call: Call) -> Return:
            seen.append((name, machine.time))
            return call.proceed()

        return record

    machine.intercept("HAL_LPTIM_AutoReloadMatchCallback", recorder("reload"))
    machine.intercept("HAL_LPTIM_CompareMatchCallback", recorder("compare"))
    return seen


async def run_for(machine: Machine, duration_ms: int) -> None:
    stop = await machine.run(for_=ms(duration_ms))
    assert stop.reason is StopReason.TIME, stop


async def test_lptim1_counts_the_lse(lptim: Machine) -> None:
    rcc = lptim.periph.RCC
    assert rcc.BDCR.LSERDY == 1
    assert rcc.CCIPR.LPTIM1SEL == LPTIM1SEL_LSE
    assert rcc.APB1ENR1.LPTIM1EN == 1
    timer = lptim.periph.LPTIM1
    assert (timer.ARR.value, timer.CMP.value) == (4095, 2047)
    assert (timer.CR.ENABLE, timer.CFGR.WAVE, timer.CFGR.CKSEL) == (1, 0, 0)


async def test_the_autoreload_match_comes_every_125_ms(lptim: Machine) -> None:
    start = lptim.time
    seen = matches(lptim)
    await run_for(lptim, 1_050)
    reloads = [at for name, at in seen if name == "reload"]
    assert len(reloads) == 8
    assert abs((reloads[0] - start).ps - FIRST_RELOAD_PS) < AROUND_CALLBACK.ps
    assert all(later - earlier == PERIOD for earlier, later in itertools.pairwise(reloads))
    assert lptim.var("periods").value == 8
    assert not lptim.periph.GPIOA.pin(5).level


async def test_the_compare_match_comes_62_5_ms_before_each_reload(lptim: Machine) -> None:
    seen = matches(lptim)
    await run_for(lptim, 1_050)
    names = [name for name, _ in seen]
    assert names == ["compare", "reload"] * 8
    for (_, compare), (_, reload) in itertools.batched(seen, 2, strict=True):
        assert abs((reload - compare).ps - PULSE_TO_RELOAD.ps) < LSE_TICK.ps
    assert lptim.var("compares").value == 8


async def test_pa5_toggles_at_each_period(lptim: Machine) -> None:
    led = lptim.periph.GPIOA.pin(5)
    levels: list[bool] = []
    await run_for(lptim, 5)
    for _ in range(4):
        await run_for(lptim, 125)
        levels.append(led.level)
    assert levels == [True, False, True, False]
    stop = await lptim.run(for_=s(1))
    assert stop.reason is StopReason.TIME, stop
    assert lptim.var("periods").value == 12
