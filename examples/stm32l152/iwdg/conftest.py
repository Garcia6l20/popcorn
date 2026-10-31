"""MachineDescription of the iwdg example."""

from pathlib import Path

import popcorn
import popcorn.pytest

HERE = Path(__file__).parent

IWDG = popcorn.MachineDescription(
    name="iwdg-refresh",
    part="STM32L152RETx",
    images=[popcorn.Elf(HERE / "prebuilt/iwdg_refresh.elf")],
    boot=popcorn.Boot(until="main", budget=1_000_000),
)


@popcorn.pytest.machine
def iwdg() -> popcorn.MachineDescription:
    return IWDG
