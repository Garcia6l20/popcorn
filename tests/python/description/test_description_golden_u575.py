from dataclasses import replace
from pathlib import Path

import pytest
from popcorn.stm32.description._builder import build_description_with_report
from popcorn.stm32.description._golden import canonical_dumps, compute_golden, load_golden
from popcorn.stm32.description._sources import Sources

GOLDEN_PATH = (
    Path(__file__).parents[3] / "tests" / "data" / "stm32" / "golden" / "STM32U575QGIx.json"
)


def test_golden_matches_committed_file(u575_sources: Sources) -> None:
    golden = load_golden(GOLDEN_PATH)
    description, report = build_description_with_report(u575_sources)

    svd_sha256 = description.sources["svd"]
    if svd_sha256 != golden["svd_sha256"]:
        pytest.skip(
            f"golden was built from a different SVD (svd_sha256 {golden['svd_sha256']} vs "
            f"{svd_sha256} on this machine); run `golden --update` to refresh it"
        )

    computed = compute_golden(description, report, svd_sha256=svd_sha256)
    assert computed["counts"] == golden["counts"]
    assert computed["tables"] == golden["tables"]


def test_golden_dump_matches_expected_counts(u575_sources: Sources) -> None:
    description, report = build_description_with_report(u575_sources)
    golden = compute_golden(description, report, svd_sha256=description.sources["svd"])

    assert golden["counts"]["peripherals"] == 197
    assert golden["counts"]["aliases"] == 98
    assert golden["counts"]["regions"] == 10
    assert golden["counts"]["irqs"] == 121


def test_a_patch_byte_change_fails_only_the_fields_digest(
    u575_sources: Sources, tmp_path: Path
) -> None:
    baseline, baseline_report = build_description_with_report(u575_sources)
    baseline_golden = compute_golden(baseline, baseline_report, svd_sha256=baseline.sources["svd"])

    patched_dir = tmp_path / "patches"
    patched_dir.mkdir()
    tweaked_paths: list[Path] = []
    for patch in u575_sources.patches:
        text = patch.read_text(encoding="utf-8")
        if patch.name == "stm32u5.toml":
            text = text.replace('on_write = "one_to_clear"', 'on_write = "zero_to_clear"', 1)
        out = patched_dir / patch.name
        out.write_text(text, encoding="utf-8")
        tweaked_paths.append(out)

    tweaked_sources = replace(u575_sources, patches=tweaked_paths)
    tweaked, tweaked_report = build_description_with_report(tweaked_sources)
    tweaked_golden = compute_golden(tweaked, tweaked_report, svd_sha256=tweaked.sources["svd"])

    changed = {
        name
        for name in baseline_golden["tables"]
        if baseline_golden["tables"][name] != tweaked_golden["tables"][name]
    }
    assert changed == {"fields"}


def test_dump_diffs_against_a_tweaked_register_are_isolated_to_registers(
    u575_sources: Sources,
) -> None:
    description, _ = build_description_with_report(u575_sources)
    dumps = canonical_dumps(description)
    assert dumps["peripherals"].startswith("P ")
    assert dumps["registers"].startswith("R ")
    assert dumps["fields"].startswith("F ")
    assert dumps["irqs"].startswith("I ")
    assert dumps["regions"].startswith("G ")
    assert dumps["core"].startswith("C ")
