"""MachineDescription of the lptim example on the STM32L476RCTx."""

from pathlib import Path

import popcorn
import popcorn.pytest

HERE = Path(__file__).parent

LPTIM = popcorn.MachineDescription(
    name="lptim",
    part="STM32L476RCTx",
    images=[popcorn.Elf(HERE / "prebuilt/lptim_lse.elf")],
    boot=popcorn.Boot(until="lptim_started", budget=2_000_000),
    strict=True,
)


@popcorn.pytest.machine
def lptim() -> popcorn.MachineDescription:
    return LPTIM
