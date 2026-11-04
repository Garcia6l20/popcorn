"""MachineDescription of the spi example on the STM32L476RCTx."""

from pathlib import Path

import popcorn
import popcorn.pytest
import pytest
from accel import Accelerometer

HERE = Path(__file__).parent

SPI = popcorn.MachineDescription(
    name="spi-accel",
    part="STM32L476RCTx",
    images=[popcorn.Elf(HERE / "prebuilt/spi_accel.elf")],
    boot=popcorn.Boot(until="main", budget=1_000_000),
    board={
        "SPI1": popcorn.Periph(
            devices=[popcorn.Device(Accelerometer, cs="PB6", counts_per_ms=100)]
        ),
    },
    strict=True,
)


@popcorn.pytest.machine
def spi() -> popcorn.MachineDescription:
    return SPI


@pytest.fixture(scope="session")
def spi_description() -> popcorn.MachineDescription:
    return SPI
