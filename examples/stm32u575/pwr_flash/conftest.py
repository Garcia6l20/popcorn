"""MachineDescription of the pwr_flash example."""

from pathlib import Path

import popcorn
import popcorn.pytest

HERE = Path(__file__).parent

PWR_FLASH = popcorn.MachineDescription(
    name="pwr-flash",
    part="STM32U575QGIx",
    images=[popcorn.Elf(HERE / "prebuilt/pwr_flash.elf")],
    boot=popcorn.Boot(until="main", budget=1_000_000),
    strict=True,
)


@popcorn.pytest.machine
def pwr_flash() -> popcorn.MachineDescription:
    return PWR_FLASH
