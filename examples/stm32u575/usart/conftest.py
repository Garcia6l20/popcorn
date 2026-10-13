"""MachineDescription of the usart example."""

from pathlib import Path

import popcorn
import popcorn.pytest
import pytest

HERE = Path(__file__).parent

PROJECT = popcorn.MachineDescription(
    name="usart-echo",
    part="STM32U575QGIx",
    images=[popcorn.Elf(HERE / "prebuilt/usart_echo.elf")],
    boot=popcorn.Boot(until="main", budget=1_000_000),
)


@popcorn.pytest.machine
def usart() -> popcorn.MachineDescription:
    return PROJECT


@pytest.fixture(scope="session")
def popcorn_project() -> popcorn.MachineDescription:
    return PROJECT
