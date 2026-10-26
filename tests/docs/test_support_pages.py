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


def test_the_h573_page_builds_without_trustzone_rows() -> None:
    from popcorn_build import support_pages

    page = support_pages.part_page(popcorn.stm32.mcu("STM32H573AIIx"))
    assert "| `RNG` |" in page
    assert support_pages.TRUSTZONE_ONLY not in page


def test_another_support_refusal_is_not_absorbed(monkeypatch: pytest.MonkeyPatch) -> None:
    from popcorn_build import support_pages

    real = support_pages.support

    def refuse(part: popcorn.PartDescription, *, trustzone: bool = False) -> object:
        if trustzone:
            raise popcorn.ConfigError("some other refusal")
        return real(part)

    monkeypatch.setattr(support_pages, "support", refuse)
    with pytest.raises(popcorn.ConfigError, match="some other refusal"):
        support_pages.part_page(popcorn.stm32.mcu("STM32U575QGIx"))
