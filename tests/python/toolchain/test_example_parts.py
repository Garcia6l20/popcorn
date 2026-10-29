"""The extra example runs: every class of the shipped parts runs firmware examples, and each
extra part fits the example it runs."""

from __future__ import annotations

import ast
import collections
from functools import cache
from pathlib import Path

import popcorn
import pytest
from popcorn.stm32 import mcu, shipped_parts

from popcorn_build.example_parts import EXAMPLE_PARTS, PartClass, extra_runs, part_class

ROOT = Path(__file__).parents[3]
EXAMPLES = ROOT / "examples"
SUITE_PARTS = ("STM32U575QGIx", "STM32H573AIIx")
FLASH_REGION = "Flash_NS"


@cache
def description(part: str) -> popcorn.PartDescription:
    return mcu(part)


def classes() -> dict[PartClass, list[str]]:
    found: dict[PartClass, list[str]] = collections.defaultdict(list)
    for part in shipped_parts():
        found[part_class(part)].append(part)
    return found


def covered() -> collections.Counter[PartClass]:
    parts = {row.conftest for row in EXAMPLE_PARTS.values()}
    parts.update(part for row in EXAMPLE_PARTS.values() for part in row.extra)
    return collections.Counter(part_class(part) for part in parts)


def bonded(part: str, pin: str) -> bool:
    port = next(p for p in description(part).peripherals if p.name == f"GPIO{pin[1]}")
    return port.bonded is not None and bool(port.bonded >> int(pin[2:]) & 1)


def flash_kib(part: str) -> int:
    region = next(r for r in description(part).regions if r.name == FLASH_REGION)
    return region.size // 1024


def test_the_classes_of_the_shipped_parts_number_51_on_two_dies() -> None:
    found = classes()
    assert len(found) == 51
    assert collections.Counter(c.die for c in found) == {"U575/U585": 30, "H56x/H573": 21}


def test_every_class_of_the_shipped_parts_runs_an_example() -> None:
    missing = sorted(set(classes()) - set(covered()))
    assert missing == []


def test_no_part_runs_for_a_class_another_run_covers() -> None:
    extra = collections.Counter(
        part_class(part) for row in EXAMPLE_PARTS.values() for part in row.extra
    )
    assert [c for c, count in extra.items() if count > 1] == []
    suite = {part_class(part) for part in SUITE_PARTS}
    assert suite.isdisjoint(extra)


def test_the_extra_parts_are_49_distinct_shipped_parts() -> None:
    parts = [part for _, part in extra_runs()]
    assert len(parts) == len(set(parts)) == 49
    assert set(parts) <= set(shipped_parts())


@pytest.mark.parametrize(
    ("example", "part"),
    [
        (example, part)
        for example, row in EXAMPLE_PARTS.items()
        for part in (row.conftest, *row.extra)
    ],
)
def test_a_part_fits_the_example_it_runs(example: str, part: str) -> None:
    row = EXAMPLE_PARTS[example]
    assert part in shipped_parts()
    assert part_class(part).die == part_class(row.conftest).die
    assert [pin for pin in row.pins if not bonded(part, pin)] == []
    if row.adc_channels:
        adc = next(p for p in description(part).peripherals if p.name == "ADC1")
        assert adc.bonded_channels is not None
        assert [c for c in row.adc_channels if not adc.bonded_channels >> c & 1] == []
    if row.flash_kib is not None:
        assert flash_kib(part) == row.flash_kib


def conftest_parts(path: Path) -> set[str]:
    found: set[str] = set()
    for node in ast.walk(ast.parse(path.read_text())):
        if isinstance(node, ast.keyword) and node.arg == "part":
            assert isinstance(node.value, ast.Constant)
            found.add(str(node.value.value))
    return found


def test_every_example_directory_has_a_row_and_its_conftest_part() -> None:
    directories = {
        f"{ini.parent.parent.name}/{ini.parent.name}" for ini in EXAMPLES.glob("*/*/pytest.ini")
    }
    assert directories == set(EXAMPLE_PARTS)
    for name, row in EXAMPLE_PARTS.items():
        assert conftest_parts(EXAMPLES / name / "conftest.py") == {row.conftest}, name
