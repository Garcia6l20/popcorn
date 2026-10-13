"""MachineDescription of the gpdma example."""

from pathlib import Path

import popcorn
import pytest

HERE = Path(__file__).parent

PROJECT = popcorn.MachineDescription(
    name="gpdma-uart",
    part="STM32U575QGIx",
    images=[popcorn.Elf(HERE / "prebuilt/gpdma_uart.elf")],
    boot=popcorn.Boot(until="main", budget=1_000_000),
)


@pytest.fixture(scope="session")
def popcorn_project() -> popcorn.MachineDescription:
    return PROJECT
