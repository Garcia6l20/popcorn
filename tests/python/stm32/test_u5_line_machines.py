from __future__ import annotations

import pytest
from popcorn import ConfigError, Machine, OptionBytes

PARTS = [
    "STM32U535CBTx",
    "STM32U535CETx",
    "STM32U535RETx",
    "STM32U535VETx",
    "STM32U545CETx",
    "STM32U545RETx",
]

FIRST_ERROR: dict[tuple[str, bool], str] = {
    (part, tzen): (
        "peripherals.GTZC1_MPCBB1" if tzen else "peripherals.LPDMA1.registers.LPDMA_MISR"
    )
    for part in PARTS
    for tzen in (False, True)
}


def build(part: str, tzen: bool) -> None:
    from popcorn.stm32 import mcu

    try:
        machine = Machine(mcu(part), option_bytes=OptionBytes(tzen=tzen))
        machine.reset()
    except ConfigError as error:
        assert (error.file, error.key) == (part, FIRST_ERROR[part, tzen]), str(error)
        raise


@pytest.mark.parametrize(
    ("part", "tzen"),
    [
        pytest.param(
            part,
            tzen,
            marks=(
                pytest.mark.xfail(strict=True, raises=ConfigError)
                if (part, tzen) in FIRST_ERROR
                else ()
            ),
            id=f"{part}-tzen{int(tzen)}",
        )
        for part in PARTS
        for tzen in (False, True)
    ],
)
def test_the_machine_of_a_u535_or_u545_part_builds(part: str, tzen: bool) -> None:
    build(part, tzen)
