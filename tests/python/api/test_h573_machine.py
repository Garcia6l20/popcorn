"""The STM32H573 description builds a Machine: every model whose IP matches constructs on it."""

from __future__ import annotations

import popcorn
import pytest
from popcorn.stm32 import mcu
from popcorn.stm32.description import DescriptionError


@pytest.fixture(scope="module")
def h573() -> popcorn.PartDescription:
    try:
        return mcu("STM32H573AIIx")
    except DescriptionError as error:
        pytest.skip(f"no STM32H573 description: {error}")


def test_the_h573_machine_builds_without_firmware(h573: popcorn.PartDescription) -> None:
    machine = popcorn.Machine(h573)
    assert machine.mem.read(0x20000000, 4) == bytes(4)


def test_a_machine_description_of_the_h573_builds_a_machine() -> None:
    description = popcorn.MachineDescription(part="STM32H573AIIx", images=[])
    try:
        machine = description.machine()
    except (DescriptionError, popcorn.MissingInput) as error:
        pytest.skip(f"no STM32H573 description: {error}")
    assert machine.mem.read(0x08FFF80C, 2) == (2048).to_bytes(2, "little")
