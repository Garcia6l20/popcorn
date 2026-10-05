"""Project of the sai example."""

from pathlib import Path

import popcorn
import pytest

HERE = Path(__file__).parent

PROJECT = popcorn.Project(
    name="sai-sine",
    part="STM32U575QGIx",
    images=[popcorn.Elf(HERE / "prebuilt/sai_sine.elf")],
    boot=popcorn.Boot(until="main", budget=1_000_000),
)


@pytest.fixture(scope="session")
def popcorn_project() -> popcorn.Project:
    return PROJECT
