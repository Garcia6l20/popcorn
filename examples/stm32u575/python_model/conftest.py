"""MachineDescription of the python_model example."""

from pathlib import Path

import popcorn
import pytest
from rngmodel import Rng

HERE = Path(__file__).parent

BASE = popcorn.MachineDescription(
    name="python-model",
    part="STM32U575QGIx",
    images=[popcorn.Elf(HERE / "prebuilt/rng_numbers.elf")],
    boot=popcorn.Boot(until="main", budget=1_000_000),
    board={"RNG": popcorn.Periph(model=popcorn.Use(Rng))},
)

PROJECT = BASE.with_configs(
    seeded=BASE.replace(board={"RNG": popcorn.Periph(model=popcorn.Use(Rng, seed=7))})
)


@pytest.fixture(scope="session")
def popcorn_project() -> popcorn.MachineDescription:
    return PROJECT
