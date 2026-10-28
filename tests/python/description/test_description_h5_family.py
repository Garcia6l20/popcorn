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
    "STM32H503RBTx": 3,
    "STM32H523CETx": 4,
    "STM32H533RETx": 2,
    "STM32H562ZITx": 28,
    "STM32H563ZITx": 28,
    "STM32H573AIIx": 27,
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


SBS_RESET_DROPS = {
    "STM32H503RBTx": 1,
    "STM32H523CETx": 0,
    "STM32H533RETx": 0,
    "STM32H562ZITx": 1,
    "STM32H563ZITx": 1,
    "STM32H573AIIx": 1,
}


@pytest.mark.timeout(60)
@pytest.mark.parametrize("part", H56X_PARTS + OTHER_PARTS)
def test_no_h5_part_keeps_the_svd_only_sbs_reset_bit(part: str) -> None:
    try:
        sources = find_sources(part)
    except DescriptionError as exc:
        pytest.skip(f"{part} sources not found: {exc}")
    loaded = load_patches(list(sources.patches), part=part, svd_name=sources.svd.name)
    drops = [
        rule
        for patch in loaded
        for rule in patch.patch.fields or []
        if rule.drop and "SBSRST" in (rule.fields or [])
    ]
    assert len(drops) == SBS_RESET_DROPS[part]
    description = build_description(sources)
    rcc = next(p for p in description.peripherals if p.name == "RCC")
    reset = next(r for r in rcc.registers if "RCC_APB3RSTR" in (r.name, *(r.aliases or [])))
    assert all(field.lsb != 1 for field in reset.fields)


@pytest.mark.timeout(60)
@pytest.mark.parametrize("part", H56X_PARTS + OTHER_PARTS)
def test_the_hash_registers_and_widths_apply_to_the_rm0481_parts_only(part: str) -> None:
    try:
        sources = find_sources(part)
    except DescriptionError as exc:
        pytest.skip(f"{part} sources not found: {exc}")
    description = build_description(sources)
    hash_block = next(p for p in description.peripherals if p.name == "HASH")
    registers = {r.name: r for r in hash_block.registers}
    status = {f.name: f.width for f in registers["SR"].fields}
    if part == "STM32H503RBTx":
        assert "HR16" not in registers
        assert "HR41" not in registers
        assert status["NBWE"] == 5
    else:
        assert registers["HR16"].offset == 0x350
        assert registers["HR41"].offset == 0x3B4
        assert status["NBWE"] == status["NBWP"] == 6


@pytest.mark.timeout(60)
@pytest.mark.parametrize("part", H56X_PARTS + OTHER_PARTS)
def test_every_h5_part_reads_sbs_mesr_mclr_1_and_clears_it_by_a_written_1(part: str) -> None:
    try:
        sources = find_sources(part)
    except DescriptionError as exc:
        pytest.skip(f"{part} sources not found: {exc}")
    description = build_description(sources)
    blocks = [p for p in description.peripherals if p.name in ("SBS", "SEC_SBS")]
    assert "SBS" in {block.name for block in blocks}
    for block in blocks:
        unit = block.name
        mesr = next(r for r in block.registers if "SBS_MESR" in (r.name, *(r.aliases or [])))
        assert mesr.reset == 1, unit
        assert {f.name: f.on_write.name.lower() for f in mesr.fields} == {
            "MCLR": "one_to_clear",
            "IPMEE": "one_to_clear",
        }, unit
