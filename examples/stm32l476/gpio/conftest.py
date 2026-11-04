"""MachineDescription of the gpio example on the STM32L476RCTx."""

from pathlib import Path

import popcorn
import popcorn.pytest
import pytest

HERE = Path(__file__).parent

GPIO = popcorn.MachineDescription(
    name="gpio-button",
    part="STM32L476RCTx",
    images=[popcorn.Elf(HERE / "prebuilt/gpio_button.elf")],
    boot=popcorn.Boot(until="main", budget=1_000_000),
    strict=True,
)


@popcorn.pytest.machine
def gpio() -> popcorn.MachineDescription:
    return GPIO


@pytest.fixture(scope="session")
def gpio_description() -> popcorn.MachineDescription:
    return GPIO
