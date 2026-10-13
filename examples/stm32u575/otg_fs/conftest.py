"""MachineDescription of the otg_fs example."""

from pathlib import Path

import popcorn
import pytest

HERE = Path(__file__).parent

PROJECT = popcorn.MachineDescription(
    name="otg-fs-init",
    part="STM32U575QGIx",
    images=[popcorn.Elf(HERE / "prebuilt/otg_fs_init.elf")],
    boot=popcorn.Boot(until="main", budget=1_000_000),
)


@pytest.fixture(scope="session")
def popcorn_project() -> popcorn.MachineDescription:
    return PROJECT
