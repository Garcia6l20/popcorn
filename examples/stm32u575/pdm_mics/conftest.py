"""MachineDescription of the pdm_mics example."""

from pathlib import Path

import popcorn
import popcorn.pytest

HERE = Path(__file__).parent

PDM_MICS = popcorn.MachineDescription(
    name="pdm-mics",
    part="STM32U575QGIx",
    images=[popcorn.Elf(HERE / "prebuilt/pdm_mics.elf")],
    boot=popcorn.Boot(until="main", budget=1_200),
)


@popcorn.pytest.machine
def pdm_mics() -> popcorn.MachineDescription:
    return PDM_MICS
