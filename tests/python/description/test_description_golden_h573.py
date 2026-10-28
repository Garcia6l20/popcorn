from pathlib import Path

import pytest
from popcorn.stm32.description._builder import build_description_with_report
from popcorn.stm32.description._golden import compute_golden, load_golden
from popcorn.stm32.description._sources import Sources

GOLDEN_PATH = (
    Path(__file__).parents[3] / "tests" / "data" / "stm32" / "golden" / "STM32H573AIIx.json"
)


def test_golden_matches_committed_file(h573_sources: Sources) -> None:
    golden = load_golden(GOLDEN_PATH)
    description, report = build_description_with_report(h573_sources)

    svd_sha256 = description.sources["svd"]
    if svd_sha256 != golden["svd_sha256"]:
        pytest.skip(
            f"golden was built from a different SVD (svd_sha256 {golden['svd_sha256']} vs "
            f"{svd_sha256} on this machine); run `golden --update` to refresh it"
        )

    computed = compute_golden(description, report, svd_sha256=svd_sha256)
    assert computed["counts"] == golden["counts"]
    assert computed["tables"] == golden["tables"]


def test_golden_dump_matches_expected_counts(h573_sources: Sources) -> None:
    description, report = build_description_with_report(h573_sources)
    golden = compute_golden(description, report, svd_sha256=description.sources["svd"])

    assert golden["counts"]["peripherals"] == 209
    assert golden["counts"]["aliases"] == 104
    assert golden["counts"]["regions"] == 12
    assert golden["counts"]["irqs"] == 131
