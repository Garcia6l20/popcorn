"""TIM2 update interrupt at 1 kHz, a TIM6 one-pulse after 50 ms and a TIM3 input capture on PA6,
HAL, on the 16 MHz HSI.

`SystemClock_Config` runs the core on HSI16 in voltage range 1 with one wait state, so every
timer counts at 16 MHz before its prescaler: TIM2 and TIM3 at 1 MHz, TIM6 at 1 kHz. Each TIM2
update increments `ticks` and toggles PA5. TIM6 runs in one-pulse mode, CR1.OPM, and its single
update records the tick count it fired at. TIM3 channel 1 captures the rising edges of PA6 on
AF2, and the capture callback keeps the count between the last two edges in `capture_period`.

The boot stops at `timers_started`, after all three timers started. TIM2 was enabled a few
instructions earlier, so its first update comes 998 us later and a 100 ms run holds exactly
99 updates. TIM6 starts after TIM2, so its update at 50 ms of its own count follows the 50th TIM2
update. The capture case reads TIM3_CNT with `read()`, a bus read that brings the count up to the
machine's time, where `.value` shows the count at the model's last access. The project sets
`MachineDescription(strict=True)`.
"""

import itertools

import pytest
from popcorn import Call, Machine, Return, StopReason
from popcorn.units import VirtualDuration, VirtualTime, ms, us

pytestmark = pytest.mark.popcorn(budget=5_000_000)

HSI = 1
FIRST_UPDATE = us(998.0)
EDGES = 4


def updates(machine: Machine) -> list[tuple[VirtualTime, bool]]:
    led = machine.periph.GPIOA.pin(5)
    seen: list[tuple[VirtualTime, bool]] = []

    def record(call: Call) -> Return:
        seen.append((machine.time, led.level))
        return call.proceed()

    machine.intercept("TIM2_IRQHandler", record)
    return seen


async def run_for(machine: Machine, duration: VirtualDuration | int) -> None:
    stop = await machine.run(for_=ms(duration) if isinstance(duration, int) else duration)
    assert stop.reason is StopReason.TIME, stop


async def test_the_core_and_the_timers_run_on_hsi16(tim: Machine) -> None:
    rcc = tim.periph.RCC
    assert rcc.CFGR.SWS == HSI
    assert (rcc.APB1ENR.TIM2EN, rcc.APB1ENR.TIM3EN, rcc.APB1ENR.TIM6EN) == (1, 1, 1)
    assert (tim.periph.TIM2.PSC.value, tim.periph.TIM2.ARR.value) == (15, 999)
    assert (tim.periph.TIM6.PSC.value, tim.periph.TIM6.ARR.value) == (15999, 49)


async def test_tim2_ticks_at_1_khz(tim: Machine) -> None:
    await run_for(tim, 100)
    assert tim.var("ticks").value == 99


async def test_each_update_comes_1_ms_after_the_last_and_toggles_pa5(tim: Machine) -> None:
    start = tim.time
    seen = updates(tim)
    await run_for(tim, 20)

    times = [at for at, _ in seen]
    assert times[0] - start == FIRST_UPDATE
    assert len(times) == 20
    assert all(later - earlier == ms(1) for earlier, later in itertools.pairwise(times))
    levels = [level for _, level in seen]
    assert levels == [index % 2 == 1 for index in range(len(levels))]


async def test_the_one_pulse_timer_fires_once_after_50_ms(tim: Machine) -> None:
    await run_for(tim, 200)
    assert tim.var("pulses").value == 1
    assert tim.var("ticks_at_pulse").value == 50
    assert tim.periph.TIM6.CR1.CEN == 0


@pytest.mark.parametrize("half_period_us", [1250, 400])
async def test_pa6_edges_are_captured_at_the_timer_count(tim: Machine, half_period_us: int) -> None:
    pin = tim.periph.GPIOA.pin(6)
    for _ in range(EDGES):
        pin.level = 1
        await run_for(tim, us(half_period_us))
        pin.level = 0
        await run_for(tim, us(half_period_us))
    assert tim.var("captures").value == EDGES
    assert tim.var("capture_period").value == 2 * half_period_us
    last_capture = tim.var("last_capture").value
    assert isinstance(last_capture, int)
    since_rising = tim.periph.TIM3.CNT.read() - last_capture
    assert since_rising & 0xFFFF == 2 * half_period_us
