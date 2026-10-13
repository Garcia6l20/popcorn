"""MachineDescription of the crc example."""

from pathlib import Path

import popcorn
import pytest

HERE = Path(__file__).parent

PROJECT = popcorn.MachineDescription(
    name="crc",
    part="STM32U575QGIx",
    images=[popcorn.Elf(HERE / "prebuilt/crc.elf")],
    boot=popcorn.Boot(until="main", budget=1_000_000),
    strict=True,
)


@pytest.fixture(scope="session")
def popcorn_project() -> popcorn.MachineDescription:
    return PROJECT
