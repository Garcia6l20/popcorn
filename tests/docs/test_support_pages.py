import re
import subprocess
import sys
from pathlib import Path

import popcorn.stm32
import pytest

from popcorn_build.example_parts import (
    EXAMPLE_PARTS,
    GEOMETRY_PARTS,
    TESTED_PARTS,
    ExampleParts,
    part_class,
)

ROOT = Path(__file__).parents[2]
MODELS = ROOT / "native/src/periph/stm32"
LIMITS = ROOT / "docs/support/limits.md"
LIMIT_SITE = re.compile(
    r"throw unmodelled_config|warn_unsupported\(|unmodelled\(|not modelled|deviation|divergence",
    re.IGNORECASE,
)


def test_the_query_runs() -> None:
    # --8<-- [start:query]
    from popcorn.stm32 import mcu, support

    unmodelled = [p.name for p in support(mcu("STM32U575QGIx")) if p.model is None]
    # --8<-- [end:query]
    assert "WWDG" in unmodelled
    assert "RNG" not in unmodelled


@pytest.fixture(scope="module")
def generated(tmp_path_factory: pytest.TempPathFactory) -> Path:
    out = tmp_path_factory.mktemp("generated")
    subprocess.run(
        [sys.executable, "-m", "popcorn_build.support_pages", str(out)], cwd=ROOT, check=True
    )
    return out


def table_instances(page: str) -> list[str]:
    rows = [line for line in page.splitlines() if line.startswith("| `")]
    return [name for row in rows for name in row.split("|")[3].strip().split(", ")]


@pytest.mark.timeout(60)
def test_every_shipped_part_has_a_page(generated: Path) -> None:
    assert sorted(p.stem for p in generated.glob("*.md")) == sorted(
        [*popcorn.stm32.shipped_parts(), "cores", "parts"]
    )


@pytest.mark.timeout(60)
def test_the_u575_page_lists_each_instance_once(generated: Path) -> None:
    part = popcorn.stm32.mcu("STM32U575QGIx")
    listed = table_instances((generated / "STM32U575QGIx.md").read_text())
    assert sorted(listed) == sorted(p.name for p in part.peripherals if p.alias_of is None)


@pytest.mark.timeout(60)
def test_the_u575_page_shows_the_rng_builtin(generated: Path) -> None:
    page = (generated / "STM32U575QGIx.md").read_text()
    assert "| `RNG` | `rng1_v4_1` | RNG | built-in |" in page


def limited_models() -> list[str]:
    sources = [*MODELS.glob("*.cpp"), *MODELS.glob("*.hpp")]
    return sorted(f.name for f in sources if LIMIT_SITE.search(f.read_text()))


def section_sources() -> dict[str, str]:
    """The `Source:` line that opens each `## ` section of limits.md, by heading."""
    found: dict[str, str] = {}
    for section in LIMITS.read_text().split("\n## ")[1:]:
        heading, _, body = section.partition("\n")
        lines = [line for line in body.splitlines() if line.strip()]
        if lines and lines[0].startswith("Source: "):
            found[heading] = lines[0]
    return found


@pytest.mark.parametrize("model", limited_models())
def test_each_model_with_a_limit_has_a_limits_section(model: str) -> None:
    link = f"native/src/periph/stm32/{model})"
    assert [heading for heading, source in section_sources().items() if link in source]


def test_the_h573_page_lists_its_gtzc_as_trustzone_only() -> None:
    from popcorn_build import support_pages

    page = support_pages.part_page(popcorn.stm32.mcu("STM32H573AIIx"))
    assert (
        "| `GTZC` | `tzsc1_v2_2_rc1_Mustang2M` | GTZC1_MPCBB1, GTZC1_MPCBB2, GTZC1_MPCBB3, "
        f"GTZC1_TZIC, GTZC1_TZSC | {support_pages.TRUSTZONE_ONLY} |" in page
    )


STATES = ("tested", "examples", "class", "attaches")
NEGATION = re.compile(r"\b(not|no|never|without|none|cannot|n't)\b", re.IGNORECASE)
EXTRA_PARTS = {part for row in EXAMPLE_PARTS.values() for part in row.extra}
GEOMETRY_NOTE = "FLASH geometry cases"


def state_rows(page: str) -> dict[str, str]:
    """The state cell of each part row of the generated parts page, by part name."""
    found: dict[str, str] = {}
    for line in page.splitlines():
        cells = [cell.strip() for cell in line.strip("|").split("|")]
        if line.startswith("| [") and len(cells) == 8:
            name = cells[0][1 : cells[0].index("]")]
            assert name not in found, name
            found[name] = cells[7]
    return found


def part_cells(page: str) -> dict[str, list[str]]:
    """The cells of each part row of the generated parts page, by part name."""
    found: dict[str, list[str]] = {}
    for line in page.splitlines():
        cells = [cell.strip() for cell in line.strip("|").split("|")]
        if line.startswith("| [") and len(cells) == 8:
            found[cells[0][1 : cells[0].index("]")]] = cells
    return found


def kind(cell: str) -> str:
    return re.split(r"[ :,]", cell, maxsplit=1)[0]


@pytest.mark.timeout(60)
def test_every_shipped_part_has_exactly_one_state_row(generated: Path) -> None:
    rows = state_rows((generated / "parts.md").read_text())
    assert sorted(rows) == sorted(popcorn.stm32.shipped_parts())
    assert all(kind(cell) in STATES for cell in rows.values())
    by_state = {state: {n for n, c in rows.items() if kind(c) == state} for state in STATES}
    assert by_state["tested"] == {"STM32U575QGIx", "STM32H573AIIx"}
    assert by_state["examples"] == EXTRA_PARTS
    assert by_state["class"] | by_state["attaches"] == set(rows) - EXTRA_PARTS - TESTED_PARTS
    assert len(by_state["examples"]) == 49
    assert len(by_state["class"]) == 43
    assert not by_state["attaches"]


@pytest.mark.timeout(60)
def test_a_class_row_names_a_part_of_its_class_that_runs_examples(generated: Path) -> None:
    rows = state_rows((generated / "parts.md").read_text())
    running = TESTED_PARTS | EXTRA_PARTS
    for name, cell in rows.items():
        if kind(cell) == "class":
            named = cell.split(" of ")[1].split(",")[0]
            assert named in running, name
            assert part_class(named) == part_class(name), name


@pytest.mark.timeout(60)
def test_the_parts_page_prints_the_line_of_the_name_and_the_die_of_the_class(
    generated: Path,
) -> None:
    page = (generated / "parts.md").read_text()
    cells = part_cells(page)
    assert "| Part | Line | Die | Package |" in page
    assert cells["STM32U585QIIx"][1:3] == ["U585", "U575/U585"]
    assert cells["STM32H562RGTx"][1:3] == ["H562", "H56x/H573"]
    for name, row in cells.items():
        assert row[1] == name[5:9], name
        assert row[2] == part_class(name.partition("-")[0]).die, name
    other_line = 0
    for name, cell in state_rows(page).items():
        if kind(cell) == "class":
            named = cell.split(" of ")[1].split(",")[0]
            other_line += cells[named][1] != cells[name][1]
            assert cells[named][2] == cells[name][2], name
    assert other_line == 43


@pytest.mark.timeout(60)
def test_the_pages_define_a_class_by_its_die(generated: Path) -> None:
    page = (generated / "parts.md").read_text()
    index = (ROOT / "docs/support/index.md").read_text()
    assert "A part's class is its die, flash size, package and SMPS supply." in page
    assert "its die, flash size, package and SMPS supply" in index.replace("\n", " ")
    assert "its line" not in page.lower()
    assert "its line" not in index.replace("\n", " ").lower()


@pytest.mark.timeout(60)
def test_a_port_with_no_bonded_pin_shows_a_dash(generated: Path) -> None:
    page = (generated / "STM32H562RGTx.md").read_text()
    assert "| GPIOI | - | 0 |" in page


def test_a_port_with_no_bonded_mask_fails_the_generator_by_name() -> None:
    from popcorn_build import support_pages

    part = popcorn.stm32.mcu("STM32H562RGTx")
    port = next(p for p in part.peripherals if p.name == "GPIOA")
    object.__setattr__(port, "bonded", None)
    with pytest.raises(ValueError, match="GPIOA"):
        support_pages.part_page(part)


def test_an_example_part_that_ships_no_description_fails_the_generator_by_name() -> None:
    from popcorn_build import support_pages

    shipped = [name for name in popcorn.stm32.shipped_parts() if name != "STM32U575VGTxQ"]
    with pytest.raises(ValueError, match="STM32U575VGTxQ"):
        support_pages.part_states(shipped)


def test_a_class_with_no_example_part_shows_attaches() -> None:
    from popcorn_build import support_pages

    dropped = "STM32U575CGTx"
    assert dropped in EXTRA_PARTS
    table = {
        name: ExampleParts(
            row.conftest,
            tuple(part for part in row.extra if part != dropped),
            row.pins,
            row.adc_channels,
            row.flash_kib,
        )
        for name, row in EXAMPLE_PARTS.items()
    }
    shipped = popcorn.stm32.shipped_parts()
    states = support_pages.part_states(shipped, examples=table)
    siblings = [n for n in shipped if part_class(n) == part_class(dropped)]
    assert dropped in siblings
    assert {states[n].state for n in siblings} == {"attaches"}
    full = support_pages.part_states(shipped)
    assert {full[n].state for n in siblings} <= {"examples", "class"}


@pytest.mark.timeout(60)
def test_a_part_that_only_the_flash_geometry_cases_run_is_never_tested(generated: Path) -> None:
    rows = state_rows((generated / "parts.md").read_text())
    assert {"STM32U575ZITxQ", "STM32H563ZGTx"} == GEOMETRY_PARTS
    assert rows["STM32U575ZITxQ"].startswith("examples: ")
    assert rows["STM32U575ZITxQ"].endswith(f", {GEOMETRY_NOTE}")
    assert rows["STM32H563ZGTx"].startswith("class of ")
    assert rows["STM32H563ZGTx"].endswith(f", {GEOMETRY_NOTE}")
    assert [n for n, c in rows.items() if GEOMETRY_NOTE in c] == sorted(GEOMETRY_PARTS)
    assert "STM32U575ZITxQ" not in TESTED_PARTS


def test_the_tested_parts_are_the_conftest_parts_of_the_examples() -> None:
    assert {"STM32U575QGIx", "STM32H573AIIx"} == TESTED_PARTS


def test_the_geometry_parts_are_the_ones_the_native_geometry_program_builds() -> None:
    build = (ROOT / "tests/native/stm32/pcons-build.py").read_text()
    named = re.findall(
        r'cached_st_description\("(STM32\w+)", "POPCORN_(?:U575_2M|H5_1M)_DESC"\)', build
    )
    assert set(named) == GEOMETRY_PARTS


@pytest.mark.timeout(60)
def test_the_parts_page_states_what_each_state_is(generated: Path) -> None:
    page = (generated / "parts.md").read_text()
    prose = "\n".join(line for line in page.splitlines() if not line.startswith("|"))
    for state in STATES:
        assert f"**{state}**" in prose
    assert "A part in the tested, examples or class state is supported." in prose
    assert "whatever its state" in prose
    assert NEGATION.findall(prose) == []
    index = (ROOT / "docs/support/index.md").read_text()
    states = index.split("## What the states mean")[1].split("## What")[0]
    assert NEGATION.findall(states) == []


@pytest.mark.timeout(60)
def test_a_part_page_lists_the_bonded_pins_of_each_port(generated: Path) -> None:
    page = (generated / "STM32U575CGTx.md").read_text()
    part = popcorn.stm32.mcu("STM32U575CGTx")
    assert "## Bonded pins" in page
    rows = {
        cells[0]: cells[1:]
        for line in page.split("## Bonded pins")[1].splitlines()
        if line.startswith("| GPIO")
        for cells in [[c.strip() for c in line.strip("|").split("|")]]
    }
    ports = [p for p in part.peripherals if p.name.startswith("GPIO") and p.alias_of is None]
    assert sorted(rows) == sorted(p.name for p in ports)
    for port in ports:
        mask = 0xFFFF if port.bonded is None else port.bonded
        assert rows[port.name][1] == str(mask.bit_count())
    assert rows["GPIOA"][0] == "PA0-PA15"
    assert rows["GPIOB"][0] == "PB0-PB10, PB12-PB15"
