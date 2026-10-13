"""MachineDescription of the spi_mock example."""

from pathlib import Path

import popcorn
import popcorn.pytest
import pytest
from spiadc import Adc

HERE = Path(__file__).parent

SPI_MOCK = popcorn.MachineDescription(
    name="spi-mock",
    part="STM32U575QGIx",
    images=[popcorn.Elf(HERE / "prebuilt/spi_adc.elf")],
    boot=popcorn.Boot(until="main", budget=1_000_000),
    board={
        "SPI1": popcorn.Periph(devices=[popcorn.Device(Adc, cs="PB0", counts_per_ms=25)]),
    },
)


@popcorn.pytest.machine
def spi_mock() -> popcorn.MachineDescription:
    return SPI_MOCK


@pytest.fixture(scope="session")
def spi_mock_description() -> popcorn.MachineDescription:
    return SPI_MOCK
