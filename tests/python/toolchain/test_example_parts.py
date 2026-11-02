"""The extra example runs: every class of the shipped parts runs firmware examples, and each
extra part fits the example it runs."""

from __future__ import annotations

import collections
import importlib.util
import struct
import sys
from collections.abc import Iterator
from functools import cache
from pathlib import Path
from typing import cast

import popcorn
import pytest
from elftools.elf.elffile import ELFFile
from elftools.elf.sections import SymbolTableSection
from popcorn.stm32 import mcu, shipped_parts

from popcorn_build.example_parts import (
    EXAMPLE_PARTS,
    L1_GROUP_A,
    PartClass,
    extra_runs,
    part_class,
)

ROOT = Path(__file__).parents[3]
EXAMPLES = ROOT / "examples"
SUITE_PARTS = ("STM32U575QGIx", "STM32H573AIIx")
FLASH_REGIONS = ("Flash_NS", "Flash", "Flash_Bank2")
SINGLE_RUN_DIES = frozenset({"U575/U585", "H56x/H573"})
L1_XE_EXTRA = ("STM32L152RCTxA", "STM32L152RCTx")
L1_XB_EXTRA = (*L1_XE_EXTRA, "STM32L152RBTxA")
VECTOR_TABLE = "g_pfnVectors"
PROGRAM_FLASH = frozenset({"Flash", "Flash_NS", "Flash_S"})
FIRST_ADC = frozenset({"ADC1", "ADC"})


@cache
def description(part: str) -> popcorn.PartDescription:
    return mcu(part)


def example_scope() -> list[str]:
    """The shipped parts the examples cover: every U5 and H5 part and the STM32L1 group A."""
    return [p for p in shipped_parts() if p.startswith(("STM32U5", "STM32H5")) or p in L1_GROUP_A]


def classes() -> dict[PartClass, list[str]]:
    found: dict[PartClass, list[str]] = collections.defaultdict(list)
    for part in example_scope():
        found[part_class(part)].append(part)
    return found


def covered() -> collections.Counter[PartClass]:
    parts = {part for row in EXAMPLE_PARTS.values() for part in (row.conftest, *row.also)}
    parts.update(part for row in EXAMPLE_PARTS.values() for part in row.extra)
    return collections.Counter(part_class(part) for part in parts)


def family(part: str) -> str:
    die = part_class(part).die
    return die if die in SINGLE_RUN_DIES else part[5:7]


def bonded(part: str, pin: str) -> bool:
    port = next(p for p in description(part).peripherals if p.name == f"GPIO{pin[1]}")
    return port.bonded is not None and bool(port.bonded >> int(pin[2:]) & 1)


def flash_kib(part: str) -> int:
    return sum(r.size for r in description(part).regions if r.name in FLASH_REGIONS) // 1024


def test_the_classes_of_the_shipped_parts_number_56_on_seven_dies() -> None:
    found = classes()
    assert len(found) == 56
    assert collections.Counter(c.die for c in found) == {
        "U575/U585": 30,
        "H56x/H573": 21,
        "DIE416": 1,
        "DIE429": 1,
        "DIE427": 1,
        "DIE436": 1,
        "DIE437": 1,
    }


def test_every_class_of_the_shipped_parts_runs_an_example() -> None:
    missing = sorted(set(classes()) - set(covered()))
    assert missing == []


def test_no_u5_or_h5_part_runs_for_a_class_another_run_covers() -> None:
    extra = collections.Counter(
        part_class(part)
        for row in EXAMPLE_PARTS.values()
        for part in row.extra
        if part_class(part).die in SINGLE_RUN_DIES
    )
    assert [c for c, count in extra.items() if count > 1] == []
    suite = {part_class(part) for part in SUITE_PARTS}
    assert suite.isdisjoint(extra)


def test_the_u5_and_h5_extra_parts_are_49_distinct_shipped_parts() -> None:
    parts = [part for _, part in extra_runs() if part_class(part).die in SINGLE_RUN_DIES]
    assert len(parts) == len(set(parts)) == 49
    assert {part for _, part in extra_runs()} <= set(shipped_parts())


def test_every_l1_example_runs_on_each_other_category_its_builds_fit() -> None:
    rows = {name: row for name, row in EXAMPLE_PARTS.items() if name.startswith("stm32l152/")}
    assert rows
    for name, row in rows.items():
        assert row.conftest == "STM32L152RETx", name
        assert row.also in ((), ("STM32L152RBTx",)), name
        assert row.extra == (L1_XB_EXTRA if row.also else L1_XE_EXTRA), name


@pytest.mark.parametrize(
    ("example", "part"),
    [
        (example, part)
        for example, row in EXAMPLE_PARTS.items()
        for part in (row.conftest, *row.also, *row.extra)
    ],
)
def test_a_part_fits_the_example_it_runs(example: str, part: str) -> None:
    row = EXAMPLE_PARTS[example]
    assert part in shipped_parts()
    assert family(part) == family(row.conftest)
    assert [pin for pin in row.pins if not bonded(part, pin)] == []
    if row.adc_channels:
        adc = next(p for p in description(part).peripherals if p.name in FIRST_ADC)
        assert adc.bonded_channels is not None
        assert [c for c in row.adc_channels if not adc.bonded_channels >> c & 1] == []
    if row.flash_kib is not None:
        assert flash_kib(part) == row.flash_kib


def test_every_example_directory_has_a_row_and_its_conftest_part() -> None:
    directories = {
        f"{ini.parent.parent.name}/{ini.parent.name}" for ini in EXAMPLES.glob("*/*/pytest.ini")
    }
    assert directories == set(EXAMPLE_PARTS)
    for name, row in EXAMPLE_PARTS.items():
        parts = {machine.part for machine in conftest_descriptions(name)}
        assert parts == {row.conftest, *row.also}, name


@pytest.mark.parametrize("part", [p for p in shipped_parts() if p.startswith("STM32L1")])
def test_an_l1_part_class_is_the_die_and_flash_of_its_description(part: str) -> None:
    found = part_class(part)
    dbgmcu = next(p for p in description(part).peripherals if p.name == "DBGMCU")
    idcode = next(r for r in dbgmcu.registers if r.name == "IDCODE")
    assert found.die == f"DIE{idcode.reset & 0xFFF:03X}"
    assert found.flash_kib == flash_kib(part)
    assert found.smps is False


@pytest.mark.timeout(60)
def test_the_l1_group_a_is_one_lqfp64_part_per_die_of_the_shipped_l1_names() -> None:
    shipped = [p for p in shipped_parts() if p.startswith("STM32L1")]
    assert set(L1_GROUP_A) <= set(shipped)
    assert {part_class(p).package for p in L1_GROUP_A} == {"LQFP64"}
    assert {part_class(p).die for p in L1_GROUP_A} == {part_class(p).die for p in shipped}
    assert len({part_class(p).die for p in L1_GROUP_A}) == len(L1_GROUP_A)


def test_the_l1_part_classes_split_the_cat3_parts_by_die() -> None:
    assert part_class("STM32L152RCTx").die == "DIE427"
    assert part_class("STM32L152RCTxA").die == "DIE436"
    assert part_class("STM32L152RBTx").die == "DIE416"
    assert part_class("STM32L152RBTxA").die == "DIE429"
    assert part_class("STM32L152RETx") == PartClass("DIE437", 512, "LQFP64", False)


def test_a_cat6_part_class_counts_both_flash_banks() -> None:
    assert part_class("STM32L151VDTxX") == PartClass("DIE437", 384, "LQFP100", False)
    assert part_class("STM32L162VDYxX") == PartClass("DIE437", 384, "WLCSP104", False)


def conftest_descriptions(example: str) -> list[popcorn.MachineDescription]:
    path = EXAMPLES / example / "conftest.py"
    spec = importlib.util.spec_from_file_location(f"conftest_{example.replace('/', '_')}", path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.path.insert(0, str(path.parent))
    try:
        spec.loader.exec_module(module)
    finally:
        sys.path.remove(str(path.parent))
    found: list[popcorn.MachineDescription] = []
    for value in vars(module).values():
        values: list[object] = (
            list(cast(dict[object, object], value).values()) if isinstance(value, dict) else [value]
        )
        found.extend(v for v in values if isinstance(v, popcorn.MachineDescription))
    return found


def elf_runs() -> Iterator[tuple[str, Path, str]]:
    for example, row in EXAMPLE_PARTS.items():
        for machine in conftest_descriptions(example):
            assert machine.part is not None, example
            for image in machine.images:
                if isinstance(image, popcorn.Elf) and image.loaded:
                    for part in (machine.part, *row.extra):
                        yield example, image.path, part


def stack_top(elf: Path) -> int:
    with elf.open("rb") as stream:
        file = ELFFile(stream)
        symbols = file.get_section_by_name(".symtab")
        vectors = file.get_section_by_name(".isr_vector")
        assert isinstance(symbols, SymbolTableSection) and vectors is not None, elf
        (table,) = symbols.get_symbol_by_name(VECTOR_TABLE) or [None]
        assert table is not None, elf
        return struct.unpack_from("<I", vectors.data(), table["st_value"] - vectors["sh_addr"])[0]


def loaded_ranges(elf: Path) -> list[tuple[int, int]]:
    with elf.open("rb") as stream:
        return [
            (segment["p_paddr"], segment["p_paddr"] + segment["p_filesz"])
            for segment in ELFFile(stream).iter_segments("PT_LOAD")
            if segment["p_filesz"]
        ]


@pytest.mark.parametrize(
    ("example", "elf", "part"),
    list(elf_runs()),
    ids=lambda value: value.name if isinstance(value, Path) else value,
)
def test_each_elf_of_an_example_fits_each_part_it_runs_on(
    example: str, elf: Path, part: str
) -> None:
    regions = description(part).regions
    top = stack_top(elf)
    ram = [r for r in regions if r.kind.name == "ram" and r.start < top <= r.start + r.size]
    assert ram, f"{example}: the stack top {top:#x} of {elf.name} is outside the SRAM of {part}"
    for start, end in loaded_ranges(elf):
        assert any(
            r.name in PROGRAM_FLASH and r.start <= start and end <= r.start + r.size
            for r in regions
        ), f"{example}: {elf.name} loads {start:#x}-{end:#x} outside the flash of {part}"
