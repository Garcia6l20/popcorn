"""ADC1 scans four external inputs on each TIM15 update, GPDMA moves the results, AWD3 guards IN6.

The firmware is HAL only. TIM15 updates every 10 ms and its TRGO starts one regular sequence:
IN1, IN2, IN5 and IN6, 12-bit, 8x oversampling shifted right by 3, so each result is the mean of
8 samples. A circular GPDMA list moves each result into `samples`, two scans deep: the half
transfer callback copies the first scan into `monitor.latest`, the complete callback the second.
AWD3 watches IN6 with a 1241 to 2482 window, 1.0 V to 2.0 V at the 3.3 V supply the project
sets. Each out-of-window result of IN6 calls `HAL_ADCEx_LevelOutOfWindow3Callback`, which counts
`monitor.alarms`.

`m.periph.ADC1.input(n).volts` drives the voltage on ADC1_INn. A conversion holds it at the end of
its sampling window, so a change counts from the next sample on. The result is the RM0456 code for
the voltage against VDDA: `volts * 16384 / 3.3` truncated to 14 bits, then shifted to 12 bits.
"""

import itertools

import pytest
from popcorn import Machine, StopReason
from popcorn.units import VirtualDuration, ms, us

pytestmark = pytest.mark.popcorn(budget=2_000_000)

VDDA_UV = 3_300_000
PERIOD = ms(10)
SCAN = us(166.1875)
COUNTING_BEFORE_SCANNING = us(0.6875)
CHANNELS = (1, 2, 5, 6)
GUARDED = 6
INSIDE = 1.5
ABOVE = 2.5
BELOW = 0.5


def code12(volts: float) -> int:
    return round(volts * 1e6) * 16384 // VDDA_UV >> 2


async def start_scanning(machine: Machine, volts: dict[int, float]) -> None:
    for channel, value in volts.items():
        machine.periph.ADC1.input(channel).volts = value
    stop = await machine.run(until="scanning")
    assert stop.reason is StopReason.ADDRESS, stop


async def run_for(machine: Machine, duration: VirtualDuration) -> None:
    stop = await machine.run(for_=duration)
    assert stop.reason is StopReason.TIME, stop


def monitor(machine: Machine) -> tuple[int, int, int, list[int]]:
    seen = machine.var("monitor")
    latest = [int(seen.latest[index].value) for index in range(len(CHANNELS))]
    return int(seen.scans.value), int(seen.alarms.value), int(seen.errors.value), latest


async def test_each_scan_lands_in_the_dma_buffer(adc_watchdog: Machine) -> None:
    volts = {1: 0.5, 2: 1.0, 5: 2.5, 6: INSIDE}
    await start_scanning(adc_watchdog, volts)
    await run_for(adc_watchdog, 3 * PERIOD + ms(5))
    codes = [code12(volts[channel]) for channel in CHANNELS]
    assert codes == [620, 1241, 3103, 1861]
    assert monitor(adc_watchdog) == (3, 0, 0, codes)
    samples = adc_watchdog.var("samples")
    assert [int(samples[index].value) for index in range(2 * len(CHANNELS))] == codes * 2


async def test_awd3_counts_each_out_of_window_result_of_in6(adc_watchdog: Machine) -> None:
    await start_scanning(adc_watchdog, {GUARDED: ABOVE})
    await run_for(adc_watchdog, 2 * PERIOD + ms(5))
    assert monitor(adc_watchdog)[:3] == (2, 2, 0)
    adc_watchdog.periph.ADC1.input(GUARDED).volts = INSIDE
    await run_for(adc_watchdog, 2 * PERIOD)
    assert monitor(adc_watchdog)[:3] == (4, 2, 0)
    adc_watchdog.periph.ADC1.input(GUARDED).volts = BELOW
    await run_for(adc_watchdog, 2 * PERIOD)
    assert monitor(adc_watchdog)[:3] == (6, 4, 0)


async def test_unguarded_inputs_driven_out_of_the_window_mid_run_raise_no_alarm(
    adc_watchdog: Machine,
) -> None:
    await start_scanning(adc_watchdog, {1: INSIDE, 2: INSIDE, 5: INSIDE, GUARDED: INSIDE})
    await run_for(adc_watchdog, PERIOD + ms(5))
    assert monitor(adc_watchdog)[:3] == (1, 0, 0)
    for channel, volts in ((1, ABOVE), (2, BELOW), (5, 3.0)):
        adc_watchdog.periph.ADC1.input(channel).volts = volts
    await run_for(adc_watchdog, 2 * PERIOD)
    scans, alarms, errors, latest = monitor(adc_watchdog)
    assert (scans, alarms, errors) == (3, 0, 0)
    assert latest == [code12(ABOVE), code12(BELOW), code12(3.0), code12(INSIDE)]


@pytest.mark.parametrize(
    ("volts", "code", "alarm"),
    [
        (2.000463, 2482, False),
        (2.000464, 2483, True),
        (0.999830, 1241, False),
        (0.999829, 1240, True),
    ],
)
async def test_awd3_flags_a_result_strictly_outside_the_thresholds(
    adc_watchdog: Machine, volts: float, code: int, alarm: bool
) -> None:
    await start_scanning(adc_watchdog, {GUARDED: volts})
    await run_for(adc_watchdog, PERIOD + ms(5))
    scans, alarms, _, latest = monitor(adc_watchdog)
    assert (scans, latest[-1]) == (1, code)
    assert alarms == int(alarm)


async def test_a_scan_ends_166_us_after_each_tim15_update(adc_watchdog: Machine) -> None:
    await start_scanning(adc_watchdog, {GUARDED: ABOVE})
    counting = adc_watchdog.time
    ends: list[int] = []
    for _ in range(3):
        stop = await adc_watchdog.run(
            for_=PERIOD + ms(1), stop_when=lambda m: m.periph.ADC1.ADC_ISR.AWD3 == 1
        )
        assert stop.reason is StopReason.USER, stop
        ends.append((adc_watchdog.time - counting).ps)
        await run_for(adc_watchdog, us(100))
    assert ends[0] == (PERIOD + SCAN - COUNTING_BEFORE_SCANNING).ps
    assert [later - earlier for earlier, later in itertools.pairwise(ends)] == [PERIOD.ps] * 2


async def test_a_snapshot_keeps_the_driven_inputs(adc_watchdog: Machine) -> None:
    await start_scanning(adc_watchdog, {GUARDED: ABOVE})
    saved = adc_watchdog.snapshot()
    adc_watchdog.periph.ADC1.input(GUARDED).volts = INSIDE
    adc_watchdog.restore(saved)
    assert adc_watchdog.periph.ADC1.input(GUARDED).volts == ABOVE
    await run_for(adc_watchdog, PERIOD + ms(5))
    assert monitor(adc_watchdog)[:3] == (1, 1, 0)
