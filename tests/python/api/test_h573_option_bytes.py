"""STM32H573 option bytes from `Machine(option_bytes=)`: the H5 FLASH model encodes them into the
_CUR option registers, RM0481 7.11, and refuses only values RM0481 cannot encode."""

from __future__ import annotations

import popcorn
import pytest
from popcorn import ConfigError, OptionBytes
from popcorn.stm32 import mcu
from popcorn.stm32.description import DescriptionError

FLASH = 0x4002_2000
OPTSR_CUR = FLASH + 0x50
OPTSR2_CUR = FLASH + 0x70
NSBOOTR_CUR = FLASH + 0x80
SECWM1R_CUR = FLASH + 0xE0
SECWM2R_CUR = FLASH + 0x1E0
HDP1R_CUR = FLASH + 0xF8


@pytest.fixture(scope="module")
def h573() -> popcorn.PartDescription:
    try:
        return mcu("STM32H573AIIx")
    except DescriptionError as error:
        pytest.skip(f"no STM32H573 description: {error}")


def test_the_h5_option_bytes_read_back_in_the_cur_registers(
    h573: popcorn.PartDescription,
) -> None:
    m = popcorn.Machine(
        h573,
        option_bytes=OptionBytes(secwm1=(0, 3), hdp1=(10, 39), nsboot=0x0800_2000, swap_bank=True),
        vector_table=0x0800_0000,
    )
    m.reset()
    assert m.mem.u32[SECWM1R_CUR] == 0x0003_0000
    assert m.mem.u32[SECWM2R_CUR] == 0x007F_0000
    assert m.mem.u32[HDP1R_CUR] == 0x0027_000A
    assert m.mem.u32[NSBOOTR_CUR] == 0x0800_20C3
    assert m.mem.u32[OPTSR_CUR] >> 31 == 1
    assert m.mem.u32[OPTSR2_CUR] >> 24 == 0xC3


@pytest.mark.parametrize(
    ("wanted", "key"),
    [
        (OptionBytes(secwm1=(0, 128)), "option_bytes.secwm1"),
        (OptionBytes(nsboot=0x0800_0080), "option_bytes.nsboot"),
        (OptionBytes(dualbank=False), "option_bytes.dualbank"),
        (OptionBytes(tzen=True), "option_bytes.tzen"),
    ],
)
def test_the_h5_refusals_are_keyed_by_their_field(
    h573: popcorn.PartDescription, wanted: OptionBytes, key: str
) -> None:
    with pytest.raises(ConfigError) as caught:
        popcorn.Machine(h573, option_bytes=wanted)
    assert caught.value.key == key


def test_dualbank_true_builds(h573: popcorn.PartDescription) -> None:
    assert popcorn.Machine(h573, option_bytes=OptionBytes(dualbank=True))
