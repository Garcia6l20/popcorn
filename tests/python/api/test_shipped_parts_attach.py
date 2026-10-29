"""Every shipped part builds a Machine from its shipped description, at TZEN 0 and 1."""

import popcorn
import pytest
from popcorn.stm32 import shipped_parts


@pytest.mark.parametrize("part", shipped_parts())
@pytest.mark.parametrize("tzen", [False, True])
def test_every_shipped_part_builds_a_machine(part: str, tzen: bool) -> None:
    option_bytes = popcorn.OptionBytes(tzen=True) if tzen else popcorn.OptionBytes()
    machine = popcorn.MachineDescription(part=part, images=[], option_bytes=option_bytes).machine()
    del machine
