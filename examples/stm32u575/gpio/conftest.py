"""MachineDescription of the gpio example."""

from pathlib import Path

import popcorn
import popcorn.pytest
import pytest

HERE = Path(__file__).parent

GPIO = popcorn.MachineDescription(
    name="gpio-button",
    part="STM32U575QGIx",
    images=[popcorn.Elf(HERE / "prebuilt/gpio_button.elf")],
    boot=popcorn.Boot(until="main", budget=1_000_000),
)


@popcorn.pytest.machine
def gpio() -> popcorn.MachineDescription:
    return GPIO


@pytest.fixture(scope="session")
def gpio_description() -> popcorn.MachineDescription:
    return GPIO
