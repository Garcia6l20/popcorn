"""Project of the pwr_flash example."""

from pathlib import Path

import popcorn
import pytest

HERE = Path(__file__).parent

PROJECT = popcorn.Project(
    name="pwr-flash",
    part="STM32U575QGIx",
    images=[popcorn.Elf(HERE / "prebuilt/pwr_flash.elf")],
    boot=popcorn.Boot(until="main", budget=1_000_000),
    strict=True,
)


@pytest.fixture(scope="session")
def popcorn_project() -> popcorn.Project:
    return PROJECT
