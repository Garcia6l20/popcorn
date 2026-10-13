"""MachineDescription of the uart_mock example."""

from pathlib import Path

import popcorn
import popcorn.pytest

HERE = Path(__file__).parent

PROJECT = popcorn.MachineDescription(
    name="uart-mock",
    part="STM32U575QGIx",
    images=[popcorn.Elf(HERE / "prebuilt/at_client.elf")],
    boot=popcorn.Boot(until="main", budget=1_000_000),
)


@popcorn.pytest.machine
def uart_mock() -> popcorn.MachineDescription:
    return PROJECT
