"""A two-channel SPI ADC mocked in Python on SPI1, read by HAL polling transfers.

`spiadc.Adc` is a `popcorn.board.SpiTarget` on the software chip select PB0. The firmware
converts both channels, 16 rounds with `HAL_Delay(1)` between them, into `adc_result.samples`.
`HAL_Delay(1)` waits one to two SysTick periods, so a round moves the default ramp by one to
two milliseconds of counts. Channel 1 is converted a few tens of microseconds after channel 0,
so its mirror lags by less than one millisecond of counts.

Two ways to attach a mock: the project file's `[board.SPI1] devices`, which the `machine`
fixture uses, and `Machine(devices={"SPI1": [adc]})`, built in the test, which can pass what a
project file cannot, here a Python function as the sample source.

A hand-built machine gets only what the test passes: here the description, the images and the
devices. It does not get the project's `[clock]` core_hz and cpi, `[models] strict`, `[boot]`
vtor, arena_base and memory_init, `[board.X] model` replacements, `[board.RCC]` clock sources
or `connect = "pty"` bridges. This project file sets none of them, so the two machines match.
`popcorn_project.boot(machine)` runs `[boot] until` within `[boot] budget`. The plugin's
failure report covers the `machine` and `machine_factory` machines only, not this one.
"""

import itertools

import pytest
from popcorn import Machine, Project, StopReason
from popcorn.units import VirtualTime, ms
from spiadc import FULL_SCALE, Adc

BUDGET = 2_000_000
pytestmark = pytest.mark.popcorn(budget=BUDGET)

CONVERSIONS = 16
CHANNELS = 2


async def run_to_done(machine: Machine) -> None:
    stop = await machine.run(until="adc_done", for_=ms(50), budget=BUDGET)
    assert stop.reason is StopReason.ADDRESS, stop


def adc(machine: Machine) -> Adc:
    (device,) = machine.board.devices("SPI1")
    assert isinstance(device, Adc)
    return device


def samples(machine: Machine, channel: int) -> list[int]:
    row = machine.var("adc_result").samples[channel]
    values = [row[index].value for index in range(CONVERSIONS)]
    assert all(isinstance(value, int) for value in values)
    return [int(value) for value in values]


async def test_the_buffer_follows_a_ramp_in_virtual_time(machine: Machine) -> None:
    await run_to_done(machine)
    assert machine.var("adc_result").conversions.value == CONVERSIONS
    step = adc(machine).counts_per_ms
    rising, mirrored = samples(machine, 0), samples(machine, 1)
    for earlier, later in itertools.pairwise(rising):
        assert step <= later - earlier < 3 * step
    for up, down in zip(rising, mirrored, strict=True):
        assert 0 <= FULL_SCALE - down - up < step


async def test_each_conversion_is_one_chip_select_period(machine: Machine) -> None:
    await run_to_done(machine)
    device = adc(machine)
    assert device.selects == device.deselects == CONVERSIONS * CHANNELS
    assert device.bad_frames == 0


async def test_a_source_function_given_through_machine_devices(
    popcorn_project: Project,
) -> None:
    served: list[tuple[int, int]] = []

    def source(channel: int, at: VirtualTime) -> int:
        value = (at.ps // ms(1).ps * 100 + channel) & FULL_SCALE
        served.append((channel, value))
        return value

    machine = Machine(
        popcorn_project.description(),
        image=popcorn_project.images(),
        devices={"SPI1": [Adc(cs="PB0", source=source)]},
    )
    await popcorn_project.boot(machine)
    await run_to_done(machine)

    assert served[0::2] == [(0, value) for value in samples(machine, 0)]
    assert served[1::2] == [(1, value) for value in samples(machine, 1)]
