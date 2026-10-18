"""MachineDescription of the trustzone example."""

from pathlib import Path

import popcorn
import popcorn.pytest
import pytest

HERE = Path(__file__).parent

TRUSTZONE = popcorn.MachineDescription(
    name="trustzone",
    part="STM32U575QGIx",
    images=[
        popcorn.Elf(HERE / "prebuilt/trustzone.elf", name="nonsecure"),
        popcorn.Elf(HERE / "prebuilt/trustzone_s.elf", name="secure", loaded=False),
    ],
    option_bytes=popcorn.OptionBytes(tzen=True, secwm1=(0, 3), secwm2=None),
    boot=popcorn.Boot(until="main", image="nonsecure", budget=100_000),
)


@popcorn.pytest.machine
def trustzone() -> popcorn.MachineDescription:
    return TRUSTZONE


@pytest.fixture(scope="session")
def trustzone_description() -> popcorn.MachineDescription:
    return TRUSTZONE
