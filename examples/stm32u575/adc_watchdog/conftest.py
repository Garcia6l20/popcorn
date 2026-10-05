"""Project of the adc_watchdog example."""

from pathlib import Path

import popcorn
import pytest
from popcorn.board import SupplyOptions

HERE = Path(__file__).parent

PROJECT = popcorn.Project(
    name="adc-watchdog",
    part="STM32U575QGIx",
    images=[popcorn.Elf(HERE / "prebuilt/adc_watchdog.elf")],
    boot=popcorn.Boot(until="main", budget=1_000_000),
    supply=SupplyOptions(vdda_mv=3300),
    strict=True,
)


@pytest.fixture(scope="session")
def popcorn_project() -> popcorn.Project:
    return PROJECT
