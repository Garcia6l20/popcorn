"""Project of the adc example."""

from pathlib import Path

import popcorn
import pytest

HERE = Path(__file__).parent

PROJECT = popcorn.Project(
    name="adc-internal",
    part="STM32U575QGIx",
    images=[popcorn.Elf(HERE / "prebuilt/adc_internal.elf")],
    boot=popcorn.Boot(until="main", budget=1_000_000),
)


@pytest.fixture(scope="session")
def popcorn_project() -> popcorn.Project:
    return PROJECT
