"""MachineDescription of the iwdg_ewi example."""

from pathlib import Path

import popcorn
import popcorn.pytest

HERE = Path(__file__).parent

PROJECT = popcorn.MachineDescription(
    name="iwdg-ewi",
    part="STM32U575QGIx",
    images=[popcorn.Elf(HERE / "prebuilt/iwdg_ewi.elf")],
    boot=popcorn.Boot(until="main", budget=1_000_000),
)


@popcorn.pytest.machine
def iwdg_ewi() -> popcorn.MachineDescription:
    return PROJECT
