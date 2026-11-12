"""The ADC12 dual mode registers of the STM32U5 come from the part's own SVD, on the parts with ADC2
only. No patch adds them."""

import tomllib
from pathlib import Path
from typing import Any

import pytest
from popcorn.stm32.description._errors import DescriptionError
from popcorn.stm32.description._sources import find_sources
from popcorn.stm32.description._svd import expand_svd

from ..conftest import require_pin_data

PATCHES = Path(__file__).parents[4] / "python" / "popcorn" / "stm32" / "description" / "patches"
DUAL_PARTS = (
    "STM32U595AIHx",
    "STM32U599NJHxQ",
    "STM32U5A5AJHx",
    "STM32U5A9NJHxQ",
    "STM32U5F7VJTx",
    "STM32U5F9NJHxQ",
    "STM32U5G7VJTx",
    "STM32U5G9NJHxQ",
)
SINGLE_PARTS = ("STM32U575QGIx", "STM32U585AIIx")
DUAL_FIELDS = {"DUAL": (0, 5), "DELAY": (8, 4), "DAMDF": (14, 2)}
DUAL_REGISTERS = {"ADC12_CSR", "ADC12_CDR", "ADC12_CDR2"}


def _common_registers(part: str) -> dict[str, dict[str, tuple[int, int]]]:
    require_pin_data()
    try:
        sources = find_sources(part)
    except DescriptionError as exc:
        pytest.skip(str(exc))
    device = expand_svd(sources.svd.read_bytes(), file=str(sources.svd))
    unit = next(p for p in device.peripherals if p.name in ("ADC12", "ADC12_Common"))
    return {r.name: {f.name: (f.lsb, f.width) for f in r.fields} for r in unit.registers}


@pytest.mark.parametrize("part", DUAL_PARTS)
def test_a_part_with_adc2_has_dual_delay_damdf_csr_and_cdr_in_its_svd(part: str) -> None:
    registers = _common_registers(part)
    fields = registers["ADC12_CCR"]
    assert {name: fields[name] for name in DUAL_FIELDS} == DUAL_FIELDS
    assert registers.keys() >= DUAL_REGISTERS


@pytest.mark.parametrize("part", SINGLE_PARTS)
def test_a_shipped_part_with_one_adc_has_none_of_them_in_its_svd(part: str) -> None:
    registers = _common_registers(part)
    assert not DUAL_FIELDS.keys() & registers["ADC12_CCR"].keys()
    assert not DUAL_REGISTERS & registers.keys()


def test_no_patch_adds_or_renames_into_a_dual_field() -> None:
    for path in PATCHES.glob("stm32u5*.toml"):
        rules: list[dict[str, Any]] = tomllib.loads(path.read_text()).get("fields", [])
        for rule in rules:
            added: set[str] = {rule["add"]["name"]} if "add" in rule else set()
            renamed: set[str] = set(rule.get("rename", {}).values())
            assert not DUAL_FIELDS.keys() & (added | renamed), path.name
