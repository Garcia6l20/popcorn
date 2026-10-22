import re
import subprocess
import sys
from pathlib import Path

import popcorn.stm32
import pytest

ROOT = Path(__file__).parents[2]
MODELS = ROOT / "native/src/periph/stm32"
LIMITS = ROOT / "docs/support/limits.md"
LIMIT_SITE = re.compile(
    r"throw unmodelled_config|warn_unsupported\(|unmodelled\(|not modelled|[Dd]eviation"
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


def test_every_shipped_part_has_a_page(generated: Path) -> None:
    assert sorted(p.stem for p in generated.glob("*.md")) == sorted(
        [*popcorn.stm32.shipped_parts(), "cores", "parts"]
    )


def test_the_u575_page_lists_each_instance_once(generated: Path) -> None:
    part = popcorn.stm32.mcu("STM32U575QGIx")
    listed = table_instances((generated / "STM32U575QGIx.md").read_text())
    assert sorted(listed) == sorted(p.name for p in part.peripherals if p.alias_of is None)


def test_the_u575_page_shows_the_rng_builtin(generated: Path) -> None:
    page = (generated / "STM32U575QGIx.md").read_text()
    assert "| `RNG` | `rng1_v4_1` | RNG | built-in |" in page


def limited_models() -> list[str]:
    return sorted(f.name for f in MODELS.glob("*.cpp") if LIMIT_SITE.search(f.read_text()))


@pytest.mark.parametrize("model", limited_models())
def test_each_model_with_a_limit_has_a_limits_section(model: str) -> None:
    assert f"native/src/periph/stm32/{model})" in LIMITS.read_text()


def test_the_u575_page_model_column_follows_support(generated: Path) -> None:
    part = popcorn.stm32.mcu("STM32U575QGIx")
    plain = {p.name: p.model for p in popcorn.stm32.support(part)}
    secured = {p.name: p.model for p in popcorn.stm32.support(part, trustzone=True)}
    expected = {
        name: (
            "built-in"
            if plain[name] == "builtin"
            else "built-in, TrustZone only" if secured[name] == "builtin" else "none"
        )
        for name in plain
    }
    rows = [
        line.split("|")
        for line in (generated / "STM32U575QGIx.md").read_text().splitlines()
        if line.startswith("| `")
    ]
    shown = {name: row[4].strip() for row in rows for name in row[3].strip().split(", ")}
    assert shown == expected
