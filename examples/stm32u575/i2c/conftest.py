"""MachineDescription of the i2c example."""

from pathlib import Path

import popcorn
import popcorn.pytest
import pytest
from popcorn.devices import Eeprom24xx, TableTarget
from popcorn.units import ms

HERE = Path(__file__).parent

EEPROM = popcorn.Device(Eeprom24xx, address=0x50, write_cycle=ms(5))
SCRIPTED = popcorn.Device(
    TableTarget,
    address=0x50,
    register_bytes=2,
    replies={0x0100: bytes(range(0xC0, 0xE0))},
)

BASE = popcorn.MachineDescription(
    name="i2c-eeprom",
    part="STM32U575QGIx",
    images=[popcorn.Elf(HERE / "prebuilt/i2c_eeprom.elf")],
    boot=popcorn.Boot(until="main", budget=1_000_000),
    board={"I2C1": popcorn.Periph(devices=[EEPROM])},
)

I2C = BASE.with_configs(
    **{
        "slow-eeprom": BASE.replace(
            board={"I2C1": popcorn.Periph(devices=[EEPROM.replace(write_cycle=ms(10))])}
        ),
        "scripted": BASE.replace(board={"I2C1": popcorn.Periph(devices=[SCRIPTED])}),
    }
)


@popcorn.pytest.machine
def i2c() -> popcorn.MachineDescription:
    return I2C


@popcorn.pytest.machine(config="scripted")
def i2c_scripted() -> popcorn.MachineDescription:
    return I2C


@pytest.fixture(scope="session")
def i2c_description() -> popcorn.MachineDescription:
    return I2C
