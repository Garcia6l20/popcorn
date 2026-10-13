"""A temperature sensor mocked in Python on I2C1, read by an LL driver every 10 ms of TIM6 time.

`tempsensor.TempSensor` is a `popcorn.board.I2CTarget`: the project attaches it at 0x48
with its scripted readings, the test reaches the same object through `machine.board.devices`.
The firmware keeps a running mean in `sensor_result` and drives PA5 high above 30 degrees.

The tests show the three uses of a mock: scripted input, the mean; what the firmware sent, the
configuration word; fault injection, NACKed addresses the firmware counts and recovers from.
Its annotated fields follow snapshots. The scripted readings are fixed at build, so they need
not.

TIM6 starts a few microseconds after `main`, so sample n comes just after n * 10 ms. Runs of
n * 10 ms that start half a period in hold exactly n samples, counted by an intercept on
`sample_taken`, which the firmware calls after each attempt, taken or failed.
"""

import pytest
from popcorn import Call, Machine, Return, StopReason
from popcorn.units import VirtualTime, ms
from tempsensor import TempSensor, counts

pytestmark = pytest.mark.popcorn(budget=2_000_000)

READINGS = [counts(value) for value in (21.0, 21.5, 22.25, 23.0, 22.5, 22.0)]
CONFIG_12BIT_4HZ = 0x60A0
SAMPLE_MS = 10


def sensor(machine: Machine) -> TempSensor:
    (device,) = machine.board.devices("I2C1")
    assert isinstance(device, TempSensor)
    return device


def attempts(machine: Machine) -> list[VirtualTime]:
    seen: list[VirtualTime] = []

    def record(call: Call) -> Return:
        seen.append(machine.time)
        return call.proceed()

    machine.intercept("sample_taken", record)
    return seen


async def run_ms(machine: Machine, duration_ms: int) -> None:
    stop = await machine.run(for_=ms(duration_ms))
    assert stop.reason is StopReason.TIME, stop


async def run_samples(machine: Machine, count: int) -> None:
    await run_ms(machine, SAMPLE_MS * count)


async def test_the_mean_follows_the_scripted_readings(i2c_mock: Machine) -> None:
    seen = attempts(i2c_mock)
    await run_ms(i2c_mock, SAMPLE_MS // 2)
    await run_samples(i2c_mock, len(READINGS))
    assert len(seen) == len(READINGS)
    result = i2c_mock.var("sensor_result")
    assert result.samples.value == len(READINGS)
    assert result.errors.value == 0
    assert result.mean.value == sum(READINGS) // len(READINGS)
    assert sensor(i2c_mock).served == len(READINGS)
    assert not i2c_mock.periph.GPIOA.pin(5).level


async def test_the_firmware_writes_the_configuration_once(i2c_mock: Machine) -> None:
    await run_ms(i2c_mock, SAMPLE_MS // 2)
    await run_samples(i2c_mock, 2)
    assert sensor(i2c_mock).config == CONFIG_12BIT_4HZ
    assert sensor(i2c_mock).config_writes == 1


async def test_nacked_addresses_are_counted_and_the_driver_recovers(i2c_mock: Machine) -> None:
    seen = attempts(i2c_mock)
    await run_ms(i2c_mock, SAMPLE_MS // 2)
    await run_samples(i2c_mock, 1)
    sensor(i2c_mock).nack_starts = 3
    await run_samples(i2c_mock, 5)
    assert len(seen) == 6
    result = i2c_mock.var("sensor_result")
    assert result.errors.value == 3
    assert result.samples.value == 3
    assert result.mean.value == sum(READINGS[:3]) // 3


async def test_the_sensor_state_follows_a_snapshot(i2c_mock: Machine) -> None:
    await run_ms(i2c_mock, SAMPLE_MS // 2)
    await run_samples(i2c_mock, 2)
    snapshot = i2c_mock.snapshot()
    await run_samples(i2c_mock, 2)
    assert sensor(i2c_mock).served == 4

    i2c_mock.restore(snapshot)
    assert sensor(i2c_mock).served == 2
    await run_samples(i2c_mock, 2)
    assert i2c_mock.var("sensor_result").mean.value == sum(READINGS[:4]) // 4


@pytest.mark.popcorn(budget=2_000_000)
async def test_a_hot_sensor_drives_the_over_temperature_pin(i2c_mock_hot: Machine) -> None:
    await run_ms(i2c_mock_hot, SAMPLE_MS // 2)
    await run_samples(i2c_mock_hot, 3)
    assert i2c_mock_hot.var("sensor_result").mean.value > 30 * 16
    assert i2c_mock_hot.periph.GPIOA.pin(5).level
