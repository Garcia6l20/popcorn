"""The model index of `docs/dev/peripherals.md` names every STM32 model source, rows header and
test file, and the family set a model source name states is the family set its rows register."""

from __future__ import annotations

import re
from pathlib import Path

import pytest

ROOT = Path(__file__).parents[2]
MODELS = ROOT / "native/src/periph/stm32"
TESTS = ROOT / "tests/native/stm32"
PAGE = ROOT / "docs/dev/peripherals.md"

FAMILIES = ("u5", "h5", "l1", "l4")
NAME = re.compile(r"`([^`]+)`")
SUFFIX = re.compile(r"_((?:u5|h5|l1|l4)+)$")
ROW_FAMILIES = re.compile(r"\.families\s*=\s*((?:family::\w+\s*\|?\s*)+)")
INSTANCE_NAMED = {"adc4", "lpgpio"}
LISTS = ("Test files that cover", "Benchmarks and test helpers")


def section(title: str) -> str:
    text = PAGE.read_text()
    start = text.index("## Model index")
    body = text[start : text.index("\n## ", start + 1)]
    parts = re.split(r"^### ", body, flags=re.MULTILINE)
    return next(part for part in parts if part.startswith(title))


def table(title: str) -> list[list[str]]:
    rows = [
        [cell.strip() for cell in line.strip().strip("|").split("|")]
        for line in section(title).splitlines()
        if line.startswith("|")
    ]
    return rows[2:]


def names(cell: str) -> list[str]:
    return NAME.findall(cell)


def model_rows() -> list[tuple[str, dict[str, list[str]], list[str]]]:
    return [
        (
            row[0],
            dict(zip(FAMILIES, (names(cell) for cell in row[2:6]), strict=True)),
            names(row[6]),
        )
        for row in table("Models")
    ]


def stems() -> dict[str, tuple[str, ...]]:
    found: dict[str, list[str]] = {}
    for _, cells, _ in model_rows():
        for family, cell in cells.items():
            for stem in cell:
                found.setdefault(stem, []).append(family)
    return {stem: tuple(families) for stem, families in found.items()}


def rows_headers() -> set[str]:
    return {header for *_, headers in model_rows() for header in headers}


def test_tables_parse() -> None:
    assert len(model_rows()) > 25
    assert len(table("Tests")) == len(model_rows())
    assert all(len(row) == 7 for row in table("Models"))
    assert all(len(row) == 5 for row in table("Tests"))


def test_a_model_stem_of_the_table_is_a_source_file() -> None:
    missing = [stem for stem in stems() if not (MODELS / f"{stem}.cpp").is_file()]
    assert not missing


def test_a_rows_header_of_the_table_is_a_file() -> None:
    missing = [name for name in rows_headers() if not (MODELS / name).is_file()]
    assert not missing


def test_every_test_file_of_the_tests_table_exists() -> None:
    named = [name for row in table("Tests") for cell in row[1:] for name in names(cell)]
    assert named
    assert not [name for name in named if not (TESTS / name).is_file()]


def test_every_other_file_of_the_page_exists() -> None:
    listing = [line for line in section("Tests").splitlines() if line.startswith(LISTS)]
    other = "\n".join(listing) + listed_line("Sources in") + listed_line("Headers that")
    named = [name for name in names(other) if "." in name]
    assert named
    missing = [
        name for name in named if not (MODELS / name).is_file() and not (TESTS / name).is_file()
    ]
    assert not missing


def listed_line(prefix: str) -> str:
    return next(line for line in section("Other sources").splitlines() if line.startswith(prefix))


def named_sources() -> set[str]:
    text = listed_line("Sources in")
    return {name for name in names(text) if name.endswith(".cpp")} | {
        f"{stem}.cpp" for stem in stems()
    }


def test_every_source_is_a_model_or_listed_as_not_one() -> None:
    present = {path.name for path in MODELS.glob("*.cpp")}
    assert present - named_sources() == set()


def test_every_header_is_a_rows_header_or_listed() -> None:
    listed = {name for name in names(listed_line("Headers that")) if name.endswith(".hpp")}
    present = {path.name for path in MODELS.glob("*.hpp")}
    assert present - listed - rows_headers() == set()
    assert not {name for name in listed if name.endswith("_rows.hpp")}


def test_every_test_file_is_named() -> None:
    present = {path.name for path in TESTS.iterdir() if path.name != "pcons-build.py"}
    assert present - set(names(section("Tests"))) == set()


def test_only_model_sources_hold_rows() -> None:
    holders = {
        path.name
        for path in MODELS.glob("*.cpp")
        if re.search(r"\b(?:row_of|flash_entry_of)<\^\^", path.read_text())
    }
    assert holders == {f"{stem}.cpp" for stem in stems()}


def families_of_rows(stem: str) -> tuple[str, ...]:
    found: set[str] = set()
    for match in ROW_FAMILIES.finditer((MODELS / f"{stem}.cpp").read_text()):
        found |= set(re.findall(r"family::(\w+)", match.group(1)))
    return tuple(family for family in FAMILIES if family in found)


@pytest.mark.parametrize("stem", sorted(stems()))
def test_the_families_of_a_model_source_are_the_families_of_its_rows(stem: str) -> None:
    ordered = tuple(family for family in FAMILIES if family in stems()[stem])
    assert ordered
    assert families_of_rows(stem) == ordered


@pytest.mark.parametrize("stem", sorted(stems()))
def test_the_name_of_a_model_source_states_its_families(stem: str) -> None:
    served = tuple(family for family in FAMILIES if family in stems()[stem])
    suffix = SUFFIX.search(stem)
    if suffix is not None:
        assert suffix.group(1) == "".join(served)
        return
    if stem in INSTANCE_NAMED:
        return
    alone = any(
        stem in {name for cell in cells.values() for name in cell}
        and len({name for cell in cells.values() for name in cell}) == 1
        for _, cells, _ in model_rows()
    )
    assert len(served) >= 3 or alone, f"{stem} serves {served} under a bare name"
