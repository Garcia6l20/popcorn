"""MachineDescription of the rcc example."""

from pathlib import Path

import popcorn
import pytest
from popcorn.board import RccOptions

HERE = Path(__file__).parent

PROJECT = popcorn.MachineDescription(
    name="rcc-clock",
    part="STM32U575QGIx",
    images=[popcorn.Elf(HERE / "prebuilt/rcc_clock.elf")],
    boot=popcorn.Boot(until="main", budget=1_000_000),
    strict=True,
    rcc=RccOptions(hse_hz=32_000_000),
)


@pytest.fixture(scope="session")
def popcorn_project() -> popcorn.MachineDescription:
    return PROJECT
