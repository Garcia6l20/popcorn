"""MachineDescription of the iwdg example on the STM32L476RCTx."""

from pathlib import Path

import popcorn
import popcorn.pytest

HERE = Path(__file__).parent

IWDG = popcorn.MachineDescription(
    name="iwdg-refresh",
    part="STM32L476RCTx",
    images=[popcorn.Elf(HERE / "prebuilt/iwdg_refresh.elf")],
    boot=popcorn.Boot(until="main", budget=1_000_000),
    strict=True,
)


@popcorn.pytest.machine
def iwdg() -> popcorn.MachineDescription:
    return IWDG
