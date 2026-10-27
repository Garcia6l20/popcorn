"""The H5 patch files against the SVD of each of the six H5 parts."""

from dataclasses import replace
from pathlib import Path

import pytest
from popcorn.stm32.description import _sources
from popcorn.stm32.description._builder import build_description, build_description_with_report
from popcorn.stm32.description._errors import DescriptionError
from popcorn.stm32.description._patches import load_patches
from popcorn.stm32.description._sources import find_sources

PATCHES = Path(_sources.__file__).parent / "patches"
H56X_PARTS = ("STM32H562ZITx", "STM32H563ZITx", "STM32H573AIIx")
OTHER_PARTS = ("STM32H503RBTx", "STM32H523CETx", "STM32H533RETx")
TIM_RENAMES = {
    "STM32H503RBTx": 6,
    "STM32H523CETx": 0,
    "STM32H533RETx": 3,
    "STM32H562ZITx": 21,
    "STM32H563ZITx": 21,
    "STM32H573AIIx": 21,
}
IRQS_ADDED = {
    "STM32H503RBTx": 2,
    "STM32H523CETx": 3,
    "STM32H533RETx": 1,
    "STM32H562ZITx": 27,
    "STM32H563ZITx": 27,
    "STM32H573AIIx": 26,
}


@pytest.mark.timeout(60)
@pytest.mark.parametrize("part", H56X_PARTS + OTHER_PARTS)
def test_the_part_files_apply_to_the_description_of_the_part(part: str) -> None:
    try:
        sources = find_sources(part)
    except DescriptionError as exc:
        pytest.skip(f"{part} sources not found: {exc}")
    _, report = build_description_with_report(sources)
    assert report.fields_renamed == TIM_RENAMES[part]
    assert report.irqs_added == IRQS_ADDED[part]


def test_the_family_wide_file_holds_no_tim_spelling_rule() -> None:
    [loaded] = load_patches(
        [PATCHES / "stm32h5.toml"], part="STM32H573AIIx", svd_name="STM32H573.svd"
    )
    assert not [rule for rule in loaded.patch.fields or [] if rule.rename]
    assert loaded.patch.irqs is None


@pytest.mark.timeout(60)
@pytest.mark.parametrize("with_header", [True, False], ids=["cmsis", "svd-only"])
@pytest.mark.parametrize("part", H56X_PARTS + OTHER_PARTS)
def test_every_h5_part_builds_from_its_own_svd(part: str, with_header: bool) -> None:
    try:
        sources = find_sources(part)
    except DescriptionError as exc:
        pytest.skip(f"{part} sources not found: {exc}")
    if with_header and sources.cmsis_header is None:
        pytest.skip(f"CMSIS header of {part} not found")
    if not with_header:
        sources = replace(sources, cmsis_header=None)
    description = build_description(sources)
    names = {p.name for p in description.peripherals}
    assert {"RCC", "FLASH", "GPIOA"} <= names
    assert not any(n.startswith("SEC_") and n[4:] not in names for n in names)
