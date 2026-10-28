"""MachineDescription of the rcc example."""

from pathlib import Path

import popcorn
import popcorn.pytest

HERE = Path(__file__).parent

RCC = popcorn.MachineDescription(
    name="rcc-clock",
    part="STM32H573AIIx",
    images=[popcorn.Elf(HERE / "prebuilt/rcc_clock.elf")],
    boot=popcorn.Boot(until="main", budget=1_000_000),
    strict=True,
)


@popcorn.pytest.machine
def rcc() -> popcorn.MachineDescription:
    return RCC
