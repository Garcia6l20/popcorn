"""The committed golden of every shipped part against a build from ST's files."""

import pytest
from popcorn.stm32.description import __main__ as cli
from popcorn.stm32.description._builder import build_description_with_report
from popcorn.stm32.description._errors import DescriptionError
from popcorn.stm32.description._golden import compute_golden, load_golden
from popcorn.stm32.description._sources import find_sources


@pytest.mark.parametrize("name", cli.golden_names())
def test_every_committed_golden_matches_a_build_from_st_files(name: str) -> None:
    part, _, core = name.partition("-")
    try:
        sources = find_sources(part, core=core or None)
    except DescriptionError as exc:
        pytest.skip(f"{name} sources not found: {exc}")
    golden = load_golden(cli.GOLDEN_DIR / f"{name}.json")
    description, report = build_description_with_report(sources)
    svd_sha256 = description.sources["svd"]
    if svd_sha256 != golden["svd_sha256"]:
        pytest.skip(f"{name}: golden built from a different SVD, run `golden --update`")
    computed = compute_golden(description, report, svd_sha256=svd_sha256)
    assert computed["counts"] == golden["counts"]
    assert computed["tables"] == golden["tables"]
