"""MachineDescription of the usart example on the STM32L476RCTx."""

from pathlib import Path

import popcorn
import popcorn.pytest
import pytest

HERE = Path(__file__).parent

USART = popcorn.MachineDescription(
    name="usart-echo",
    part="STM32L476RCTx",
    images=[popcorn.Elf(HERE / "prebuilt/usart_echo.elf")],
    boot=popcorn.Boot(until="main", budget=1_000_000),
    strict=True,
)


@popcorn.pytest.machine
def usart() -> popcorn.MachineDescription:
    return USART


@pytest.fixture(scope="session")
def usart_description() -> popcorn.MachineDescription:
    return USART
