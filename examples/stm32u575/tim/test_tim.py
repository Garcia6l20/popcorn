"""TIM2 update interrupt at 1 kHz and a TIM16 one-pulse after 50 ms, LL.

Timers run from the 4 MHz reset clock, prescaled to 1 MHz for TIM2 and 1 kHz for TIM16. Each
TIM2 update increments `ticks` and toggles PA5, and the single TIM16 update records the tick
count it fired at.

The setup code after `main` takes 19 us at 4 MHz before TIM2 counts, so the first update comes
1.019 ms after the boot snapshot. A 100 ms run from `main` therefore holds exactly 99 updates.
TIM16 starts a few instructions after TIM2, so its update at 50 ms of its own count follows
the 50th TIM2 update.
"""

import itertools

import pytest
from popcorn import Call, Machine, Return, StopReason
from popcorn.units import VirtualTime, ms

pytestmark = pytest.mark.popcorn(budget=5_000_000)


def updates(machine: Machine) -> list[tuple[VirtualTime, bool]]:
    led = machine.periph.GPIOA.pin(5)
    seen: list[tuple[VirtualTime, bool]] = []

    def record(call: Call) -> Return:
        seen.append((machine.time, led.level))
        return call.proceed()

    machine.intercept("TIM2_IRQHandler", record)
    return seen


async def run_for(machine: Machine, duration_ms: int) -> None:
    stop = await machine.run(for_=ms(duration_ms))
    assert stop.reason is StopReason.TIME, stop


async def test_tim2_ticks_at_1_khz(machine: Machine) -> None:
    await run_for(machine, 100)
    assert machine.var("ticks").value == 99


async def test_each_update_comes_1_ms_after_the_last_and_toggles_pa5(machine: Machine) -> None:
    seen = updates(machine)
    await run_for(machine, 20)

    times = [at for at, _ in seen]
    assert len(times) >= 19
    assert all(later - earlier == ms(1) for earlier, later in itertools.pairwise(times))
    levels = [level for _, level in seen]
    assert levels == [index % 2 == 1 for index in range(len(levels))]


async def test_the_one_pulse_timer_fires_once_after_50_ms(machine: Machine) -> None:
    await run_for(machine, 200)
    assert machine.var("pulses").value == 1
    assert machine.var("ticks_at_pulse").value == 50
