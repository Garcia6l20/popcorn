"""MachineDescription of the python_model example."""

from pathlib import Path

import popcorn
import popcorn.pytest
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

PYTHON_MODEL = BASE.with_configs(
    seeded=BASE.replace(board={"RNG": popcorn.Periph(model=popcorn.Use(Rng, seed=7))})
)


@popcorn.pytest.machine
def python_model() -> popcorn.MachineDescription:
    return PYTHON_MODEL


@popcorn.pytest.machine(config="seeded")
def python_model_seeded() -> popcorn.MachineDescription:
    return PYTHON_MODEL


@pytest.fixture(scope="session")
def python_model_description() -> popcorn.MachineDescription:
    return PYTHON_MODEL
