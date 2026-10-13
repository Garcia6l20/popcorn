"""MachineDescription of the tim example."""

from pathlib import Path

import popcorn
import popcorn.pytest

HERE = Path(__file__).parent

PROJECT = popcorn.MachineDescription(
    name="tim-ticks",
    part="STM32U575QGIx",
    images=[popcorn.Elf(HERE / "prebuilt/tim_ticks.elf")],
    boot=popcorn.Boot(until="main", budget=1_000_000),
)


@popcorn.pytest.machine
def tim() -> popcorn.MachineDescription:
    return PROJECT
