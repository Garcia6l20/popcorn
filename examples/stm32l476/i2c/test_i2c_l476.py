"""A temperature sensor mocked in Python on I2C1, read by the HAL every 10 ms of SysTick time.

`thermometer.Thermometer` is a `popcorn.board.I2CTarget` at 0x48 on I2C1, PB8 SCL and PB9 SDA on
AF4. The firmware writes the configuration register once with `HAL_I2C_Mem_Write`, then reads the
2-byte temperature register with `HAL_I2C_Mem_Read`: a pointer write, a repeated START and a 2-byte
read through the STM32L4 I2C, RM0351 39: NBYTES and AUTOEND in I2C_CR2, TXIS and RXNE in I2C_ISR,
and TC after the register address. I2C1 runs from PCLK1, I2C1SEL 00, at the 4 MHz MSI reset clock,
and I2C_TIMINGR 0x00420F13 is the Standard-mode column of RM0351 Table 240, 8 MHz, with PRESC one
lower for half the clock. It keeps a running mean in `sensor_result` and drives PA5 high above 30
degrees.

The firmware samples when SysTick reaches each 10 ms step after `main`, so a run of n * 10 ms
that starts half a period in holds exactly n samples, counted by an intercept on
`sample_taken`, called after each attempt, taken or failed. A NACKed address makes the HAL
return HAL_ERROR with HAL_I2C_ERROR_AF, from ISR.NACKF, and the firmware counts it and goes on.
The project sets `MachineDescription(strict=True)`.
"""

import pytest
from popcorn import Call, Machine, MachineDescription, Return, StopReason
from popcorn.pytest import MachineFactory
from popcorn.units import VirtualTime, ms
from thermometer import Thermometer, counts

pytestmark = pytest.mark.popcorn(budget=2_000_000)

READINGS = [counts(value) for value in (21.0, 21.5, 22.25, 23.0, 22.5, 22.0)]
CONFIG_12BIT_4HZ = 0x60A0
SAMPLE_MS = 10
AF_OPEN_DRAIN = (2, 1)
HAL_I2C_ERROR_AF = 0x04
TIMING_SM_AT_4MHZ = 0x00420F13


def sensor(machine: Machine) -> Thermometer:
    (device,) = machine.board.devices("I2C1")
    assert isinstance(device, Thermometer)
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


async def test_the_mean_follows_the_scripted_readings(i2c: Machine) -> None:
    seen = attempts(i2c)
    await run_ms(i2c, SAMPLE_MS // 2)
    await run_samples(i2c, len(READINGS))
    assert len(seen) == len(READINGS)
    result = i2c.var("sensor_result")
    assert result.samples.value == len(READINGS)
    assert result.errors.value == 0
    assert result.mean.value == sum(READINGS) // len(READINGS)
    assert sensor(i2c).served == len(READINGS)
    assert not i2c.periph.GPIOA.pin(5).level


async def test_the_firmware_writes_the_configuration_once(i2c: Machine) -> None:
    await run_ms(i2c, SAMPLE_MS // 2)
    await run_samples(i2c, 2)
    assert sensor(i2c).config == CONFIG_12BIT_4HZ
    assert sensor(i2c).config_writes == 1


async def test_the_msp_init_enables_i2c1_and_pb8_pb9_open_drain_on_af4(i2c: Machine) -> None:
    assert i2c.periph.RCC.APB1ENR1.I2C1EN == 0
    await run_ms(i2c, SAMPLE_MS // 2)
    assert i2c.periph.RCC.APB1ENR1.I2C1EN == 1
    assert i2c.periph.I2C1.TIMINGR.value == TIMING_SM_AT_4MHZ
    gpiob = i2c.periph.GPIOB
    assert (gpiob.MODER.MODER8, gpiob.OTYPER.OT8) == AF_OPEN_DRAIN
    assert (gpiob.MODER.MODER9, gpiob.OTYPER.OT9) == AF_OPEN_DRAIN
    assert (gpiob.AFRH.AFRH8, gpiob.AFRH.AFRH9) == (4, 4)


async def test_nacked_addresses_are_counted_and_the_driver_recovers(i2c: Machine) -> None:
    seen = attempts(i2c)
    await run_ms(i2c, SAMPLE_MS // 2)
    await run_samples(i2c, 1)
    sensor(i2c).nack_starts = 3
    await run_samples(i2c, 5)
    assert len(seen) == 6
    result = i2c.var("sensor_result")
    assert result.errors.value == 3
    assert result.last_error.value == HAL_I2C_ERROR_AF
    assert result.samples.value == 3
    assert result.mean.value == sum(READINGS[:3]) // 3


async def test_the_sensor_state_follows_a_snapshot(i2c: Machine) -> None:
    await run_ms(i2c, SAMPLE_MS // 2)
    await run_samples(i2c, 2)
    snapshot = i2c.snapshot()
    await run_samples(i2c, 2)
    assert sensor(i2c).served == 4

    i2c.restore(snapshot)
    assert sensor(i2c).served == 2
    await run_samples(i2c, 2)
    assert i2c.var("sensor_result").mean.value == sum(READINGS[:4]) // 4


async def test_a_hot_sensor_drives_the_over_temperature_pin(i2c_hot: Machine) -> None:
    await run_ms(i2c_hot, SAMPLE_MS // 2)
    await run_samples(i2c_hot, 3)
    assert i2c_hot.var("sensor_result").mean.value > 30 * 16
    assert i2c_hot.periph.GPIOA.pin(5).level


async def test_readings_given_through_the_factory(
    machine_factory: MachineFactory, i2c_description: MachineDescription
) -> None:
    machine = await machine_factory.booted(
        i2c_description, devices={"I2C1": [Thermometer(0x48, readings=(-5.0, -4.0))]}
    )
    await run_ms(machine, SAMPLE_MS // 2)
    await run_samples(machine, 2)
    assert machine.var("sensor_result").mean.value == (-5 * 16 - 4 * 16) // 2
