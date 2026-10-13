"""MachineDescription of the i2c_mock example."""

from pathlib import Path

import popcorn
import popcorn.pytest
from tempsensor import TempSensor

HERE = Path(__file__).parent

COLD = [21.0, 21.5, 22.25, 23.0, 22.5, 22.0]
HOT = [34.0, 35.5, 36.0]

BASE = popcorn.MachineDescription(
    name="i2c-mock",
    part="STM32U575QGIx",
    images=[popcorn.Elf(HERE / "prebuilt/i2c_sensor.elf")],
    boot=popcorn.Boot(until="main", budget=1_000_000),
    board={
        "I2C1": popcorn.Periph(devices=[popcorn.Device(TempSensor, address=0x48, readings=COLD)])
    },
)

PROJECT = BASE.with_configs(
    hot=BASE.replace(
        board={
            "I2C1": popcorn.Periph(devices=[popcorn.Device(TempSensor, address=0x48, readings=HOT)])
        }
    )
)


@popcorn.pytest.machine
def i2c_mock() -> popcorn.MachineDescription:
    return PROJECT


@popcorn.pytest.machine(config="hot")
def i2c_mock_hot() -> popcorn.MachineDescription:
    return PROJECT
