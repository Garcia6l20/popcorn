"""Projects and stand-in model classes shared by the api tests."""

from __future__ import annotations

from pathlib import Path

import popcorn
from popcorn.board import I2CTarget
from popcorn.devices import At25sf
from popcorn.model import Model
from popcorn.units import us

from .devices_example import Eeprom24

ROOT = Path(__file__).parents[3]
PREBUILT = ROOT / "tests" / "firmware" / "periph" / "prebuilt"
MODEL_JSON = ROOT / "tests" / "data" / "api" / "model.json"
MODEL_ELF = ROOT / "tests" / "firmware" / "api" / "prebuilt" / "model.elf"
BUDGET = 100_000_000


class Eeprom(I2CTarget):
    """A bus device."""

    def __init__(self, address: int, size: int) -> None:
        super().__init__(address)
        self.size = size


class Timer(Model):
    """A peripheral model."""

    def __init__(self, prescaler: int = 1) -> None:
        super().__init__()
        self.prescaler = prescaler


class Silent(Model):
    """A Python model with no register behaviour, standing in for an SAI."""


class Plain:
    """A class that is not a model."""


DEVICES = popcorn.MachineDescription(
    name="devices",
    part="STM32U575QGIx",
    images=[popcorn.Elf(PREBUILT / "i2c_ll.elf")],
    boot=popcorn.Boot(until="main", budget=BUDGET),
    board={"I2C1": popcorn.Periph(devices=[popcorn.Device(Eeprom24, address=0x50, size=4096)])},
)

NATIVE_DEVICES = popcorn.MachineDescription(
    name="native-devices",
    part="STM32U575QGIx",
    images=[popcorn.Elf(PREBUILT / "spi_ll.elf")],
    boot=popcorn.Boot(until="main", budget=BUDGET),
    board={
        "SPI1": popcorn.Periph(
            devices=[
                popcorn.Device(At25sf, jedec=[0x1F, 0x87, 0x01], size=4194304, page_program=us(600))
            ]
        )
    },
)


def model_project(board: dict[str, popcorn.Periph]) -> popcorn.MachineDescription:
    return popcorn.MachineDescription(
        part="POPCORN-M33-TEST",
        description=MODEL_JSON,
        images=[popcorn.Elf(MODEL_ELF)],
        board=board,
    )
