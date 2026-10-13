"""MachineDescription of the sai example."""

from pathlib import Path

import popcorn
import popcorn.pytest

HERE = Path(__file__).parent

SAI = popcorn.MachineDescription(
    name="sai-sine",
    part="STM32U575QGIx",
    images=[popcorn.Elf(HERE / "prebuilt/sai_sine.elf")],
    boot=popcorn.Boot(until="main", budget=1_000_000),
)


@popcorn.pytest.machine
def sai() -> popcorn.MachineDescription:
    return SAI
