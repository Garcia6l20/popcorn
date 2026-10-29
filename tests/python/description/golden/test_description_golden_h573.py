from pathlib import Path

from popcorn.stm32.description._builder import build_description_with_report
from popcorn.stm32.description._golden import compute_golden
from popcorn.stm32.description._sources import Sources

GOLDEN_PATH = (
    Path(__file__).parents[4] / "tests" / "data" / "stm32" / "golden" / "STM32H573AIIx.json"
)


def test_golden_dump_matches_expected_counts(h573_sources: Sources) -> None:
    description, report = build_description_with_report(h573_sources)
    golden = compute_golden(description, report, svd_sha256=description.sources["svd"])

    assert golden["counts"]["peripherals"] == 209
    assert golden["counts"]["aliases"] == 104
    assert golden["counts"]["regions"] == 12
    assert golden["counts"]["irqs"] == 131
