"""Shared inputs of the facade tests: the core test description and firmware."""

from __future__ import annotations

from pathlib import Path

import popcorn
import pytest

ROOT = Path(__file__).parents[3]
M33_JSON = ROOT / "tests" / "data" / "core" / "m33.json"
CORE_FIRMWARE = ROOT / "tests" / "firmware" / "core" / "prebuilt"
DRIVER_ELF = ROOT / "tests" / "firmware" / "api" / "prebuilt" / "driver.elf"


@pytest.fixture
def m33_json() -> Path:
    """Path of the core test description."""
    return M33_JSON


@pytest.fixture
def core_firmware() -> Path:
    """Directory of the prebuilt core test firmware."""
    return CORE_FIRMWARE


@pytest.fixture
def m33() -> popcorn.MachineDescription:
    """The core test description, a Cortex-M33 with flash, SRAM and test peripherals."""
    return popcorn.load_description(M33_JSON)


@pytest.fixture
def basic() -> popcorn.Image:
    """The basic core test firmware."""
    return popcorn.Image.elf(CORE_FIRMWARE / "basic.elf")


@pytest.fixture
def driver() -> popcorn.Image:
    """The driver test firmware: an ALU loop from reset, other scenarios behind `vt_*` tables."""
    return popcorn.Image.elf(DRIVER_ELF)


class Machines:
    """Builds reset machines over the driver firmware, one scenario each."""

    def __init__(self, description: popcorn.MachineDescription, image: popcorn.Image) -> None:
        self.description = description
        self.image = image

    def __call__(
        self,
        scenario: str | None = None,
        *,
        spin_threshold: int = 0,
        slice_instructions: int = 1_000_000,
        slice_wall_budget: float = 1e-3,
    ) -> popcorn.Machine:
        """A machine reset into `scenario`, the vector table `vt_<scenario>`, or the ALU loop."""
        table = None if scenario is None else self.image.lookup(f"vt_{scenario}").value
        m = popcorn.Machine(
            self.description,
            image=self.image,
            vector_table=table,
            spin_threshold=spin_threshold,
            slice_instructions=slice_instructions,
            slice_wall_budget=slice_wall_budget,
        )
        m.reset()
        return m


@pytest.fixture
def machines(m33: popcorn.MachineDescription, driver: popcorn.Image) -> Machines:
    """Factory of reset machines over the driver firmware."""
    return Machines(m33, driver)
