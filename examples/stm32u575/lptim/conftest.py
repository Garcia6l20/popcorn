"""MachineDescription of the lptim example on the STM32U575QGIx."""

from pathlib import Path

import popcorn
import popcorn.pytest

HERE = Path(__file__).parent

LPTIM = popcorn.MachineDescription(
    name="lptim-msik",
    part="STM32U575QGIx",
    images=[popcorn.Elf(HERE / "prebuilt/lptim_msik.elf")],
    boot=popcorn.Boot(until="lptim_started", budget=1_000_000),
    strict=True,
)


@popcorn.pytest.machine
def lptim() -> popcorn.MachineDescription:
    return LPTIM
