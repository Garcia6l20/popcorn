"""The scope of the LPGPIO1_Py alternate patches against the pin data of every U575 and U585."""

from pathlib import Path

import pytest
from popcorn.stm32.description._cubemx import bonded_pins, read_part
from popcorn.stm32.description._patches import load_patches

from ..conftest import require_pin_data
from ..lpgpio1_pads import LPGPIO1_PADS

PATCH_DIR = Path(__file__).parents[4] / "python" / "popcorn" / "stm32" / "description" / "patches"


def _parts() -> list[str]:
    root = require_pin_data()
    names = sorted(p.stem for p in (root / "mcu").glob("STM32U5[78]*.xml"))
    assert len(names) == 45
    return names


def test_every_u575_and_u585_part_adds_the_lpgpio1_pads_it_bonds_and_only_those() -> None:
    root = require_pin_data()
    files = sorted(PATCH_DIR.glob("stm32u5_lpgpio1*.toml"))
    assert len(files) == 5
    for part in _parts():
        bonded = bonded_pins(read_part((root / "mcu" / f"{part}.xml").read_bytes(), file=part))
        expected = {pad for pad in LPGPIO1_PADS if bonded.get(pad[1], 0) >> int(pad[2:]) & 1}
        added: list[str] = []
        for loaded in load_patches(files, part=part, svd_name=f"{part[:9]}.svd"):
            assert loaded.patch.alternates is not None
            for rule in loaded.patch.alternates.add or []:
                assert rule.af == 11
                assert rule.signal == f"lpgpio1_p{LPGPIO1_PADS[f'P{rule.peripheral[4]}{rule.pin}']}"
                added.append(f"P{rule.peripheral[4]}{rule.pin}")
        assert sorted(added) == sorted(expected), part
        assert len(added) == len(set(added)), part


@pytest.mark.parametrize(
    "part",
    [
        "STM32U535CBTx",
        "STM32U535VETx",
        "STM32U545RETx",
        "STM32U545CETxQ",
        "STM32U595AJHx",
        "STM32U595QJIx",
        "STM32U599ZIYxQ",
        "STM32U5A5ZJTx",
        "STM32U5F9NJHxQ",
        "STM32U5G7VJTx",
        "STM32H573AIIx",
    ],
)
def test_no_other_part_matches_an_lpgpio1_patch(part: str) -> None:
    files = sorted(PATCH_DIR.glob("stm32u5_lpgpio1*.toml"))
    assert load_patches(files, part=part, svd_name="x.svd") == []
