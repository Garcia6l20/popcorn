"""Every shipped part builds a Machine from its shipped description with no option bytes, and at
TZEN 0 with the default option bytes. At TZEN 1 a part with TrustZone builds one and a part without
it is refused."""

import popcorn
import pytest
from popcorn.stm32 import mcu, shipped_parts


@pytest.mark.parametrize("part", shipped_parts())
@pytest.mark.parametrize("tzen", [False, True])
def test_every_shipped_part_builds_a_machine(part: str, tzen: bool) -> None:
    trustzone = mcu(part).core.trustzone
    description = popcorn.MachineDescription(
        part=part, images=[], option_bytes=popcorn.OptionBytes(tzen=tzen)
    )
    if not tzen:
        plain = popcorn.MachineDescription(part=part, images=[]).machine()
        del plain
    if tzen and not trustzone:
        with pytest.raises(popcorn.ConfigError, match=r"option_bytes\.tzen"):
            description.machine()
        return
    machine = description.machine()
    del machine
