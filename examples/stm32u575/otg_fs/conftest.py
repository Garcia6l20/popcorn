"""MachineDescription of the otg_fs example."""

from pathlib import Path

import popcorn
import popcorn.pytest

HERE = Path(__file__).parent

OTG_FS = popcorn.MachineDescription(
    name="otg-fs-init",
    part="STM32U575QGIx",
    images=[popcorn.Elf(HERE / "prebuilt/otg_fs_init.elf")],
    boot=popcorn.Boot(until="main", budget=1_000_000),
)


@popcorn.pytest.machine
def otg_fs() -> popcorn.MachineDescription:
    return OTG_FS
