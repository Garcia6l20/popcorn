"""The H5 patch files against the SVD of each of the six H5 parts."""

import json
import os
from dataclasses import replace
from pathlib import Path

import pytest
from popcorn.stm32.description import _sources
from popcorn.stm32.description._builder import build_description, build_description_with_report
from popcorn.stm32.description._errors import DescriptionError
from popcorn.stm32.description._patches import load_patches
from popcorn.stm32.description._shipped import build_text
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


SMPS_PARTS = [
    "STM32H563AIIxQ",
    "STM32H563IIKxQ",
    "STM32H563IITxQ",
    "STM32H563LIHxQ",
    "STM32H563MIYxQ",
    "STM32H563VITxQ",
    "STM32H563ZITxQ",
    "STM32H573AIIxQ",
    "STM32H573IIKxQ",
    "STM32H573IITxQ",
    "STM32H573LIHxQ",
    "STM32H573MIYxQ",
    "STM32H573VITxQ",
    "STM32H573ZITxQ",
]
LDO_PARTS = [
    "STM32H562AGIx",
    "STM32H562AIIx",
    "STM32H562IGKx",
    "STM32H562IGTx",
    "STM32H562IIKx",
    "STM32H562IITx",
    "STM32H562RGTx",
    "STM32H562RGVx",
    "STM32H562RITx",
    "STM32H562RIVx",
    "STM32H562VGTx",
    "STM32H562VITx",
    "STM32H562ZGTx",
    "STM32H562ZITx",
    "STM32H563AGIx",
    "STM32H563AIIx",
    "STM32H563IGKx",
    "STM32H563IGTx",
    "STM32H563IIKx",
    "STM32H563IITx",
    "STM32H563RGTx",
    "STM32H563RGVx",
    "STM32H563RITx",
    "STM32H563RIVx",
    "STM32H563VGTx",
    "STM32H563VITx",
    "STM32H563ZGTx",
    "STM32H563ZITx",
    "STM32H573AIIx",
    "STM32H573IIKx",
    "STM32H573IITx",
    "STM32H573RITx",
    "STM32H573RIVx",
    "STM32H573VITx",
    "STM32H573ZITx",
]
SUPPLY_FILES = ("stm32h56x_smps.toml", "stm32h56x_ldo.toml")
SUPPLY_BUILD_PARTS = (
    "STM32H562RGTx",
    "STM32H563ZITx",
    "STM32H563ZITxQ",
    "STM32H573AIIx",
    "STM32H573ZITxQ",
)
SCCR_SMPSEN = 0x200
SCCR_LDOEN = 0x100


def _supply_files_of(part: str) -> list[str]:
    files = [PATCHES / name for name in SUPPLY_FILES]
    return [p.path.name for p in load_patches(files, part=part, svd_name="STM32H5xx.svd")]


def test_the_supply_files_cover_the_49_h5_group_a_parts_once_each() -> None:
    assert len(SMPS_PARTS) + len(LDO_PARTS) == 49
    assert len(set(SMPS_PARTS + LDO_PARTS)) == 49
    for part in SMPS_PARTS:
        assert _supply_files_of(part) == ["stm32h56x_smps.toml"], part
    for part in LDO_PARTS:
        assert _supply_files_of(part) == ["stm32h56x_ldo.toml"], part


@pytest.mark.parametrize("part", OTHER_PARTS)
def test_the_supply_files_leave_the_h503_h523_h533_parts_alone(part: str) -> None:
    assert _supply_files_of(part) == []


def _sccr_resets(text: str) -> dict[str, tuple[int, int]]:
    data = json.loads(text)
    found: dict[str, tuple[int, int]] = {}
    for peripheral in data["peripherals"]:
        if peripheral["name"] in ("PWR", "SEC_PWR"):
            register = next(r for r in peripheral["registers"] if r["name"] == "SCCR")
            found[peripheral["name"]] = (register["reset"], register["reset_mask"])
    return found


def _pin_data() -> Path:
    value = os.environ.get("POPCORN_STM32_PIN_DATA")
    if not value:
        pytest.skip("set POPCORN_STM32_PIN_DATA to a STM32_open_pin_data checkout")
    return Path(value)


@pytest.mark.timeout(60)
@pytest.mark.parametrize("part", SUPPLY_BUILD_PARTS)
def test_pwr_sccr_resets_to_the_supply_the_package_uses(part: str) -> None:
    expected = SCCR_SMPSEN if part in SMPS_PARTS else SCCR_LDOEN
    resets = _sccr_resets(build_text(part, pin_data=_pin_data()))
    assert resets == {"PWR": (expected, 0xFFFFFFFF), "SEC_PWR": (expected, 0xFFFFFFFF)}
