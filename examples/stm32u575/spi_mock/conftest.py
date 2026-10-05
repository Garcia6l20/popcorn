"""Project of the spi_mock example."""

from pathlib import Path

import popcorn
import pytest
from spiadc import Adc

HERE = Path(__file__).parent

PROJECT = popcorn.Project(
    name="spi-mock",
    part="STM32U575QGIx",
    images=[popcorn.Elf(HERE / "prebuilt/spi_adc.elf")],
    boot=popcorn.Boot(until="main", budget=1_000_000),
    board={
        "SPI1": popcorn.Periph(devices=[popcorn.Device(Adc, cs="PB0", counts_per_ms=25)]),
    },
)


@pytest.fixture(scope="session")
def popcorn_project() -> popcorn.Project:
    return PROJECT
