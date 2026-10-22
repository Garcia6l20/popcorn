"""MachineDescription of the rng example."""

from pathlib import Path

import popcorn
import popcorn.pytest
import pytest
from popcorn.board import RngOptions

HERE = Path(__file__).parent

BASE = popcorn.MachineDescription(
    name="rng",
    part="STM32U575QGIx",
    images=[popcorn.Elf(HERE / "prebuilt/rng_key.elf")],
    boot=popcorn.Boot(until="main", budget=1_000_000),
    strict=True,
)

RNG = BASE.with_configs(seeded=BASE.replace(rng=RngOptions(seed=0x5EED)))


@popcorn.pytest.machine
def rng() -> popcorn.MachineDescription:
    return RNG


@popcorn.pytest.machine(config="seeded")
def rng_seeded() -> popcorn.MachineDescription:
    return RNG


@pytest.fixture(scope="session")
def rng_description() -> popcorn.MachineDescription:
    return RNG
