"""MachineDescription of the rcc example on the STM32L476RCTx."""

from pathlib import Path

import popcorn
import popcorn.pytest

HERE = Path(__file__).parent

RCC = popcorn.MachineDescription(
    name="rcc-clock",
    part="STM32L476RCTx",
    images=[popcorn.Elf(HERE / "prebuilt/rcc_clock.elf")],
    boot=popcorn.Boot(until="main", budget=1_000_000),
    strict=True,
)


@popcorn.pytest.machine
def rcc() -> popcorn.MachineDescription:
    return RCC
