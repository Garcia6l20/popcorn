import fcntl
import os
import re
import subprocess
import sys
import tempfile
from functools import cache
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


def _generate(out: Path) -> str | None:
    result = subprocess.run(
        [sys.executable, "-m", "popcorn_build.support_pages", str(out)],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    if result.returncode == 0:
        return None
    return f"popcorn_build.support_pages exited {result.returncode}\n{result.stderr}"


@pytest.fixture(scope="session")
def generated(tmp_path_factory: pytest.TempPathFactory) -> Path:
    """The generated pages, written once per session. Under xdist the first worker to take the
    lock in the session's shared temporary directory generates into a fresh directory there,
    renames it to `support_pages`, or writes the generator's error to `support_pages.failed`,
    which every worker then reports without generating again."""
    if os.environ.get("PYTEST_XDIST_WORKER") is None:
        out = tmp_path_factory.mktemp("generated")
        error = _generate(out)
        if error is not None:
            pytest.fail(error, pytrace=False)
        return out
    shared = tmp_path_factory.getbasetemp().parent
    out = shared / "support_pages"
    failed = shared / "support_pages.failed"
    with (shared / "support_pages.lock").open("w") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        if not out.exists() and not failed.exists():
            work = Path(tempfile.mkdtemp(dir=shared))
            error = _generate(work)
            if error is None:
                work.rename(out)
            else:
                failed.write_text(error, encoding="utf-8")
    if failed.exists():
        pytest.fail(failed.read_text(encoding="utf-8"), pytrace=False)
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


L1_NAMES = [name for name in popcorn.stm32.shipped_parts() if name.startswith("STM32L1")]
L4_NAMES = [name for name in popcorn.stm32.shipped_parts() if name.startswith("STM32L4")]
L1_CLASS = 11
L1_ATTACHES = 116
L4_CLASS = {"STM32L475RCTx"}
STATES = ("tested", "examples", "class", "attaches")
NEGATION = re.compile(r"\b(not|no|never|without|none|cannot|n't)\b", re.IGNORECASE)
EXTRA_PARTS = {part for row in EXAMPLE_PARTS.values() for part in row.extra}
GEOMETRY_NOTE = "FLASH geometry cases"


def state_rows(page: str) -> dict[str, str]:
    """The state cell of each part row of the generated parts page, by part name."""
    found: dict[str, str] = {}
    for line in page.splitlines():
        cells = [cell.strip() for cell in line.strip("|").split("|")]
        if line.startswith("| [") and len(cells) == 9:
            name = cells[0][1 : cells[0].index("]")]
            assert name not in found, name
            found[name] = cells[8]
    return found


def part_cells(page: str) -> dict[str, list[str]]:
    """The cells of each part row of the generated parts page, by part name."""
    found: dict[str, list[str]] = {}
    for line in page.splitlines():
        cells = [cell.strip() for cell in line.strip("|").split("|")]
        if line.startswith("| [") and len(cells) == 9:
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
    assert by_state["tested"] == TESTED_PARTS
    assert by_state["examples"] == EXTRA_PARTS
    assert by_state["class"] | by_state["attaches"] == set(rows) - EXTRA_PARTS - TESTED_PARTS
    in_family = {
        state: {n for n in names if n.startswith(("STM32U5", "STM32H5"))}
        for state, names in by_state.items()
    }
    assert len(in_family["examples"]) == 49
    assert len(in_family["class"]) == 43
    assert not in_family["attaches"]
    assert by_state["tested"] & set(L1_NAMES) == {"STM32L152RETx", "STM32L152RBTx"}
    assert len(by_state["examples"] & set(L1_NAMES)) == 3
    assert len(by_state["class"] & set(L1_NAMES)) == L1_CLASS
    assert len(by_state["attaches"] & set(L1_NAMES)) == L1_ATTACHES
    assert by_state["tested"] & set(L4_NAMES) == {"STM32L476RCTx"}
    assert by_state["class"] & set(L4_NAMES) == L4_CLASS
    assert by_state["attaches"] & set(L4_NAMES) == set(L4_NAMES) - {"STM32L476RCTx"} - L4_CLASS


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
    assert "| Part | Family | Line | Die | Package |" in page
    assert cells["STM32U585QIIx"][1:4] == ["STM32U5", "U585", "U575/U585"]
    assert cells["STM32H562RGTx"][1:4] == ["STM32H5", "H562", "H56x/H573"]
    assert cells["STM32L152RETx"][1:4] == ["STM32L1", "L152", "DIE437"]
    for name, row in cells.items():
        assert row[1] == name[:7], name
        assert row[2] == name[5:9], name
        assert row[3] == part_class(name.partition("-")[0]).die, name
    other_line = 0
    for name, cell in state_rows(page).items():
        if kind(cell) == "class":
            named = cell.split(" of ")[1].split(",")[0]
            other_line += cells[named][2] != cells[name][2] and not name.startswith(
                ("STM32L1", "STM32L4")
            )
            assert cells[named][3] == cells[name][3], name
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
    assert {
        "STM32U575QGIx",
        "STM32H573AIIx",
        "STM32L152RETx",
        "STM32L152RBTx",
        "STM32L476RCTx",
    } == TESTED_PARTS
    assert {
        part for row in EXAMPLE_PARTS.values() for part in (row.conftest, *row.also)
    } == TESTED_PARTS


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


@pytest.mark.timeout(60)
def test_the_parts_page_lists_every_l1_name_with_its_family_and_state(generated: Path) -> None:
    cells = part_cells((generated / "parts.md").read_text())
    assert len(L1_NAMES) == 132
    assert [name for name in cells if name.startswith("STM32L1")] == L1_NAMES
    assert {cells[name][1] for name in L1_NAMES} == {"STM32L1"}
    families = {cells[name][1] for name in cells if name not in L1_NAMES}
    assert families == {"STM32U5", "STM32H5", "STM32L4"}
    assert [cells[name][8] for name in L4_NAMES] == [
        (
            "tested"
            if name == "STM32L476RCTx"
            else "class of STM32L476RCTx" if name in L4_CLASS else "attaches"
        )
        for name in L4_NAMES
    ]
    assert cells["STM32L152RETx"][8] == "tested"
    assert cells["STM32L152RCTxA"][8].startswith("examples: ")


@pytest.mark.timeout(60)
def test_an_l1_page_lists_each_instance_once_with_its_model(generated: Path) -> None:
    part = popcorn.stm32.mcu("STM32L152RETx")
    page = (generated / "STM32L152RETx.md").read_text()
    assert sorted(table_instances(page)) == sorted(
        p.name for p in part.peripherals if p.alias_of is None
    )
    assert "| `Flash` | `STM32L1_flash_cat5` | FLASH | built-in |" in page
    assert "Core CM3." in page


@pytest.mark.timeout(60)
def test_the_cores_page_lists_the_cortex_m3_parts(generated: Path) -> None:
    page = (generated / "cores.md").read_text()
    assert "| STM32L152RETx | CM3 | no | no | no | 8 | 57 |" in page
    assert "`CM33`, `CM3`" in page
    assert (
        sum(line.startswith("| STM32L1") and "| CM3 |" in line for line in page.splitlines()) == 132
    )


L1_MODEL_SOURCES = (
    "adc_l1.cpp",
    "flash_l1.cpp",
    "exti_classic.cpp",
    "dma_classic.cpp",
    "i2c_v1.cpp",
    "rtc_classic.cpp",
    "spi_classic.cpp",
    "syscfg_classic.cpp",
    "crc.cpp",
    "dbgmcu.cpp",
    "gpio.cpp",
    "iwdg.cpp",
    "pwr.cpp",
    "rcc.cpp",
    "rcc_l1.hpp",
    "tim.cpp",
    "usart.cpp",
    "usb_drd.cpp",
)


def section_bodies() -> dict[str, str]:
    return {
        heading: body
        for section in LIMITS.read_text().split("\n## ")[1:]
        for heading, _, body in [section.partition("\n")]
    }


@pytest.mark.parametrize("model", L1_MODEL_SOURCES)
def test_each_l1_model_has_a_limits_section_that_covers_the_stm32l1(model: str) -> None:
    link = f"native/src/periph/stm32/{model})"
    bodies = [body for body in section_bodies().values() if link in body.split("\n\n")[0]]
    assert len(bodies) == 1
    assert "STM32L1" in bodies[0]


@pytest.mark.timeout(60)
def test_the_l1_page_states_the_option_bytes_the_family_accepts() -> None:
    page = (ROOT / "docs/support/stm32l1.md").read_text()
    assert "| `swap_bank` | Cat.4, Cat.5 and Cat.6 codes |" in page
    assert "option_bytes.<field>" in page
    assert "[STM32L1](stm32l1.md)" in (ROOT / "docs/support/index.md").read_text()
    assert "support/stm32l1.md" in (ROOT / "zensical.toml").read_text()


L1_PAGE = ROOT / "docs/support/stm32l1.md"
L1_CATEGORY = {
    ("DIE416", 32): "Cat.1",
    ("DIE416", 64): "Cat.1",
    ("DIE416", 128): "Cat.1",
    ("DIE429", 32): "Cat.2",
    ("DIE429", 64): "Cat.2",
    ("DIE429", 128): "Cat.2",
    ("DIE427", 256): "Cat.3",
    ("DIE436", 256): "Cat.3",
    ("DIE436", 384): "Cat.4",
    ("DIE437", 512): "Cat.5",
    ("DIE437", 384): "Cat.6",
}
"""Category of a die and flash size, RM0038 Rev 18 1.4 and 30.6.1 DEV_ID."""


@cache
def _l1_classes() -> dict[str, tuple[str, int]]:
    return {name: (part_class(name).die, part_class(name).flash_kib) for name in L1_NAMES}


def _l1_categories() -> dict[str, list[str]]:
    categories: dict[str, list[str]] = {}
    for name, found in _l1_classes().items():
        categories.setdefault(L1_CATEGORY[found], []).append(name)
    return categories


def _region_kib(part: popcorn.PartDescription, name: str) -> int:
    return sum(r.size for r in part.regions if r.name == name) // 1024


def _kib(values: set[int], joiner: str) -> str:
    ordered = sorted(values)
    if len(ordered) == 1:
        return f"{ordered[0]} KiB"
    if joiner == ", ":
        return f"{', '.join(str(v) for v in ordered)} KiB"
    return f"{ordered[0]} {joiner} {ordered[-1]} KiB"


def _page_text() -> str:
    return " ".join(L1_PAGE.read_text().split())


@pytest.mark.timeout(60)
def test_the_l1_category_table_is_the_shipped_data() -> None:
    rows = {
        cells[0]: cells[1:]
        for line in L1_PAGE.read_text().splitlines()
        if line.startswith("| Cat.")
        for cells in [[cell.strip() for cell in line.strip("|").split("|")]]
    }
    expected: dict[str, list[str]] = {}
    for category, names in sorted(_l1_categories().items()):
        parts = [popcorn.stm32.mcu(name) for name in names]
        expected[category] = [
            ", ".join(sorted({_l1_classes()[name][0] for name in names})),
            _kib({_l1_classes()[name][1] for name in names}, ", "),
            _kib({_region_kib(part, "SRAM") for part in parts}, "to"),
            _kib({_region_kib(part, "DATA_EEPROM") for part in parts}, "or"),
            str(len(names)),
        ]
    assert rows == expected


def test_the_l1_page_gives_the_cat6_banks_of_the_shipped_data() -> None:
    for name in _l1_categories()["Cat.6"]:
        banks = [r for r in popcorn.stm32.mcu(name).regions if r.name.startswith("Flash")]
        first, second = banks
        hole = (second.start - first.start - first.size) // 1024
        sentence = (
            f"Cat.6 codes carry two flash regions, `{first.name}` of {first.size // 1024} KiB at "
            f"{first.start:#010x} and `{second.name}` of {second.size // 1024} KiB at "
            f"{second.start:#010x}. The {hole} KiB between them is unmapped."
        )
        assert sentence in _page_text(), name


def test_the_l1_page_names_the_categories_that_accept_swap_bank() -> None:
    accepted: list[str] = []
    for category, names in sorted(_l1_categories().items()):
        try:
            popcorn.Machine(
                popcorn.stm32.mcu(names[0]), option_bytes=popcorn.OptionBytes(swap_bank=True)
            )
        except popcorn.ConfigError:
            continue
        accepted.append(category)
    cell = f"{', '.join(accepted[:-1])} and {accepted[-1]} codes"
    assert f"| `swap_bank` | {cell} |" in L1_PAGE.read_text()
