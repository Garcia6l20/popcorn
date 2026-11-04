"""A three-axis accelerometer mocked in Python on SPI1, read by HAL polling transfers in mode 3.

`accel.Accelerometer` is a `popcorn.board.SpiTarget` on the software chip select PB6, declared in
SPI mode 3. The firmware reads WHO_AM_I, writes CTRL_REG1, then reads the six output registers
in one auto-increment transfer, 8 times with `HAL_Delay(1)` between them, into
`accel_result.samples`. `HAL_Delay(1)` waits one to two SysTick periods, so the X ramp moves by
one to two milliseconds of counts between samples.

SPI1 runs from PCLK2, the 4 MHz MSI reset clock, divided by 4. The L4 SPI has a 32-bit FIFO and
a programmable frame size, RM0351 42.4.6. `HAL_SPI_Init` sets DS 0111 for 8-bit frames and FRXTH
1, so RXNE rises at each received byte. The project sets
`MachineDescription(strict=True)`, so any register access the SPI model does not serve fails the
run. A device built with another mode than the firmware's CPOL and CPHA ends the run with
`ConfigError`.
"""

import itertools

import pytest
from accel import IDENTITY, ONE_G, Accelerometer
from popcorn import ConfigError, Machine, MachineDescription, StopReason
from popcorn.pytest import MachineFactory
from popcorn.units import ms

BUDGET = 2_000_000
pytestmark = pytest.mark.popcorn(budget=BUDGET)

SAMPLES = 8
AXES = 3
CTRL_VALUE = 0x57
DS_8_BIT = 0b0111
MISMATCH = r"the SPI mode 0 .*, not mode 3 the firmware set, CPOL 1 CPHA 1"


async def run_to_done(machine: Machine) -> None:
    stop = await machine.run(until="accel_done", for_=ms(50), budget=BUDGET)
    assert stop.reason is StopReason.ADDRESS, stop


def accelerometer(machine: Machine) -> Accelerometer:
    (device,) = machine.board.devices("SPI1")
    assert isinstance(device, Accelerometer)
    return device


def samples(machine: Machine) -> list[tuple[int, ...]]:
    rows = machine.var("accel_result").samples
    out: list[tuple[int, ...]] = []
    for index in range(SAMPLES):
        values = [rows[index][axis].value for axis in range(AXES)]
        assert all(isinstance(value, int) for value in values)
        out.append(tuple(int(value) for value in values))
    return out


async def test_the_identity_and_the_control_register(spi: Machine) -> None:
    await run_to_done(spi)
    assert spi.var("accel_result").who_am_i.value == IDENTITY
    assert accelerometer(spi).ctrl_reg1 == CTRL_VALUE


async def test_the_samples_follow_the_ramp_in_virtual_time(spi: Machine) -> None:
    await run_to_done(spi)
    assert spi.var("accel_result").count.value == SAMPLES
    step = accelerometer(spi).counts_per_ms
    rows = samples(spi)
    for x, y, z in rows:
        assert y == -x
        assert z == ONE_G
    for (earlier, *_), (later, *_) in itertools.pairwise(rows):
        assert step <= later - earlier < 3 * step


async def test_each_transfer_is_one_chip_select_period(spi: Machine) -> None:
    await run_to_done(spi)
    device = accelerometer(spi)
    assert device.selects == 2 + SAMPLES
    assert device.samples == SAMPLES


async def test_the_msp_init_enables_the_spi1_clock_and_pa5_to_pa7_on_af5(spi: Machine) -> None:
    assert spi.periph.RCC.APB2ENR.SPI1EN == 0
    await run_to_done(spi)
    assert spi.periph.RCC.APB2ENR.SPI1EN == 1
    gpioa = spi.periph.GPIOA
    assert (gpioa.AFRL.AFRL5, gpioa.AFRL.AFRL6, gpioa.AFRL.AFRL7) == (5, 5, 5)
    assert (spi.periph.SPI1.CR1.CPOL, spi.periph.SPI1.CR1.CPHA) == (1, 1)


async def test_8_bit_frames_use_ds_7_and_the_quarter_fifo_threshold(spi: Machine) -> None:
    await run_to_done(spi)
    cr2 = spi.periph.SPI1.CR2
    assert (cr2.DS, cr2.FRXTH) == (DS_8_BIT, 1)
    assert spi.periph.SPI1.SR.FRLVL == 0


async def test_a_device_in_another_mode_ends_the_run(
    machine_factory: MachineFactory, spi_description: MachineDescription
) -> None:
    machine = await machine_factory.booted(
        spi_description, devices={"SPI1": [Accelerometer(cs="PB6", mode=0)]}
    )
    with pytest.raises(ConfigError, match=MISMATCH):
        await machine.run(until="accel_done", for_=ms(50), budget=BUDGET)
