"""MachineDescription of the i2c example."""

from pathlib import Path

import popcorn
import popcorn.pytest
import pytest
from thermometer import Thermometer

HERE = Path(__file__).parent

COLD = [21.0, 21.5, 22.25, 23.0, 22.5, 22.0]
HOT = [34.0, 35.5, 36.0]


def board(readings: list[float]) -> dict[str, popcorn.Periph]:
    return {
        "I2C1": popcorn.Periph(
            devices=[popcorn.Device(Thermometer, address=0x48, readings=readings)]
        )
    }


BASE = popcorn.MachineDescription(
    name="i2c-sensor",
    part="STM32L152RETx",
    images=[popcorn.Elf(HERE / "prebuilt/i2c_sensor.elf")],
    boot=popcorn.Boot(until="main", budget=1_000_000),
    board=board(COLD),
    strict=True,
)

I2C = BASE.with_configs(hot=BASE.replace(board=board(HOT)))


@popcorn.pytest.machine
def i2c() -> popcorn.MachineDescription:
    return I2C


@popcorn.pytest.machine(config="hot")
def i2c_hot() -> popcorn.MachineDescription:
    return I2C


@pytest.fixture(scope="session")
def i2c_description() -> popcorn.MachineDescription:
    return BASE
