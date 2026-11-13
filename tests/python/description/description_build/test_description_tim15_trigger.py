"""The TIM15 trigger select is spelled TS on every STM32U5 description."""

from pathlib import Path

import pytest
from popcorn.stm32 import mcu
from popcorn.stm32.description import _sources
from popcorn.stm32.description._patches import load_patches

PATCHES = Path(_sources.__file__).parent / "patches"
PARTS = (
    "STM32U535CBTx",
    "STM32U545CETx",
    "STM32U575QGIx",
    "STM32U585QIIx",
    "STM32U595AIHx",
    "STM32U5A5AJHx",
    "STM32U5F7VJTx",
    "STM32U5G7VJTx",
)


@pytest.mark.timeout(120)
@pytest.mark.parametrize("part", PARTS)
def test_tim15_smcr_has_a_three_bit_ts_field_at_bit_4(part: str) -> None:
    tim15 = next(p for p in mcu(part).peripherals if p.name == "TIM15")
    smcr = next(r for r in tim15.registers if r.name == "SMCR")
    field = next((f for f in smcr.fields if f.name == "TS"), None)
    assert field is not None
    assert (field.lsb, field.width) == (4, 3)


@pytest.mark.parametrize(
    ("part", "svd", "applied"),
    [
        ("STM32U535CBTx", "STM32U535.svd", True),
        ("STM32U545CETx", "STM32U545.svd", False),
        ("STM32U575QGIx", "STM32U575.svd", False),
        ("STM32U5G7VJTx", "STM32U5G7.svd", False),
    ],
)
def test_the_ts_rename_applies_to_the_u535_svd_only(part: str, svd: str, applied: bool) -> None:
    loaded = load_patches([PATCHES / "stm32u535.toml"], part=part, svd_name=svd)
    assert bool(loaded) is applied
