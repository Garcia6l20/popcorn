"""LPTIM1 on MSIK / 128, LL: an autoreload match every 100 ms and a compare match half way through
each period.

The firmware enables MSIK with RCC_CR.MSIKON, selects it as the LPTIM1 kernel clock through
RCC_CCIPR3.LPTIM1SEL, RM0456 11.8.33, and divides it by 128 with CFGR.PRESC 111, RM0456 58.4.6:
4 MHz / 128 = 31.25 kHz, one count every 32 us. With ARR 3124 the counter wraps every 3125
counts, 100 ms. CC1IF sets when the counter reaches CCR1 1562, ARRM when it reaches ARR, RM0456
58.7.2, 1562 counts, 49.984 ms, apart. Each enable, ARR and CCR1 write waits for its DIEROK,
ARROK or CMP1OK flag, RM0456 58.4.11. The ARRM interrupt counts `periods` and toggles PA5, the
CC1IF interrupt counts `compares`. The project sets `MachineDescription(strict=True)`.
"""

import itertools
from collections.abc import Callable

import pytest
from popcorn import Call, Machine, Return, StopReason
from popcorn.units import VirtualTime, ms, us

pytestmark = pytest.mark.popcorn(budget=5_000_000)

PERIOD = ms(100)
COMPARE_TO_RELOAD = us(32 * 1562)
LPTIM1SEL_MSIK = 0b00
PRESC_128 = 0b111


def matches(machine: Machine) -> list[tuple[str, VirtualTime]]:
    seen: list[tuple[str, VirtualTime]] = []

    def recorder(name: str) -> Callable[[Call], Return]:
        def record(call: Call) -> Return:
            seen.append((name, machine.time))
            return call.proceed()

        return record

    machine.intercept("LPTIM1_IRQHandler", recorder("irq"))
    return seen


async def run_for(machine: Machine, duration_ms: int) -> None:
    stop = await machine.run(for_=ms(duration_ms))
    assert stop.reason is StopReason.TIME, stop


async def test_lptim1_counts_msik_divided_by_128(lptim: Machine) -> None:
    rcc = lptim.periph.RCC
    assert rcc.CR.MSIKRDY == 1
    assert rcc.CCIPR3.LPTIM1SEL == LPTIM1SEL_MSIK
    assert rcc.APB3ENR.LPTIM1EN == 1
    timer = lptim.periph.LPTIM1
    assert (timer.ARR.value, timer.CCR1.value) == (3124, 1562)
    assert (timer.CR.ENABLE, timer.CFGR.PRESC) == (1, PRESC_128)


async def test_the_interrupts_alternate_compare_then_reload_every_100_ms(lptim: Machine) -> None:
    seen = matches(lptim)
    await run_for(lptim, 1_020)
    times = [at for _, at in seen]
    assert len(times) == 20
    compares, reloads = times[0::2], times[1::2]
    assert all(later - earlier == PERIOD for earlier, later in itertools.pairwise(reloads))
    assert all(later - earlier == PERIOD for earlier, later in itertools.pairwise(compares))
    assert all(
        reload - compare == COMPARE_TO_RELOAD
        for compare, reload in zip(compares, reloads, strict=True)
    )
    assert (lptim.var("compares").value, lptim.var("periods").value) == (10, 10)


async def test_pa5_toggles_at_each_period(lptim: Machine) -> None:
    led = lptim.periph.GPIOA.pin(5)
    levels: list[bool] = []
    await run_for(lptim, 105)
    for _ in range(4):
        levels.append(led.level)
        await run_for(lptim, 100)
    assert levels == [True, False, True, False]
