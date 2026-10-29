"""Every shipped part builds a Machine from its shipped description at TZEN 0. At TZEN 1 a part
with TrustZone builds one and a part without it is refused. A part without TrustZone takes no
option bytes from the host, so its TZEN 0 Machine is built with none."""

import popcorn
import pytest
from popcorn.stm32 import mcu, shipped_parts


@pytest.mark.parametrize("part", shipped_parts())
@pytest.mark.parametrize("tzen", [False, True])
def test_every_shipped_part_builds_a_machine(part: str, tzen: bool) -> None:
    trustzone = mcu(part).core.trustzone
    if tzen:
        description = popcorn.MachineDescription(
            part=part, images=[], option_bytes=popcorn.OptionBytes(tzen=True)
        )
    elif trustzone:
        description = popcorn.MachineDescription(
            part=part, images=[], option_bytes=popcorn.OptionBytes()
        )
    else:
        description = popcorn.MachineDescription(part=part, images=[])
    if tzen and not trustzone:
        with pytest.raises(popcorn.ConfigError, match=r"option_bytes\.tzen"):
            description.machine()
        return
    machine = description.machine()
    del machine
