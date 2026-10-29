"""Shipped descriptions against a fresh ship build, when STM32_open_pin_data is installed. A
builder, patch or facts change that changes a shipped part's content fails here until
`python -m popcorn.stm32.description ship` reruns."""

import json
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path
from typing import cast

import popcorn
import pytest
from popcorn.stm32.description import __main__ as cli
from popcorn.stm32.description import _shipped
from popcorn.stm32.description._errors import DescriptionError, MissingSource
from popcorn.stm32.description._shipped import compose_shipped, shipped_names
from popcorn.stm32.description._sources import compute_source_digests, pin_data_sources

from ..conftest import require_pin_data
from ..shipped.test_description_shipped_u575 import REFERENCES

EXTERNAL_SOURCES = ("svd", "mcu_db", "cmsis_header", "gpio_modes")
"""Source digests of files popcorn does not own. A difference there means another ST install,
not a stale shipped file."""


def _sources_of(name: str) -> dict[str, str]:
    data = cast(dict[str, object], json.loads(compose_shipped(name)))
    return cast(dict[str, str], data["sources"])


@pytest.mark.parametrize("name", shipped_names())
def test_the_shipped_description_matches_a_fresh_ship_build(name: str) -> None:
    root = require_pin_data()
    part, _, core = name.partition("-")
    try:
        sources = pin_data_sources(part, pin_data=root, core=core or None)
    except MissingSource as exc:
        pytest.skip(f"{name} sources not found: {exc}")
    local = compute_source_digests(sources)
    shipped_sources = _sources_of(name)
    stale = [
        key for key in local if key.startswith("patch:") and local[key] != shipped_sources.get(key)
    ]
    assert not stale, f"{name}: shipped patch digests differ, rerun ship: {stale}"
    assert {k for k in shipped_sources if k.startswith("patch:")} == {
        k for k in local if k.startswith("patch:")
    }
    differing = [key for key in EXTERNAL_SOURCES if local.get(key) != shipped_sources.get(key)]
    if differing:
        pytest.skip(f"{name}: local {', '.join(differing)} differ from the shipped file's")
    fresh = popcorn.parse_description(_shipped.build_text(name, pin_data=root), "fresh")
    shipped = popcorn.parse_description(compose_shipped(name), name)
    assert popcorn.description_digest(fresh) == popcorn.description_digest(shipped)


@pytest.mark.parametrize("name", sorted(REFERENCES.values()))
def test_ship_check_passes_on_the_shipped_layers(
    name: str, capsys: pytest.CaptureFixture[str]
) -> None:
    root = require_pin_data()
    try:
        pin_data_sources(name, pin_data=root)
    except MissingSource as exc:
        pytest.skip(f"{name} sources not found: {exc}")
    assert cli.main(["ship", name, "--pin-data", str(root), "--check"]) == 0, capsys.readouterr()


def test_ship_check_builds_the_fresh_texts_in_a_pool_of_jobs_processes(
    monkeypatch: pytest.MonkeyPatch, capsys: pytest.CaptureFixture[str]
) -> None:
    root = require_pin_data()
    names = sorted(REFERENCES.values())[:2]
    for name in names:
        try:
            pin_data_sources(name, pin_data=root)
        except MissingSource as exc:
            pytest.skip(f"{name} sources not found: {exc}")
    pools: list[int] = []

    class Pool(ProcessPoolExecutor):
        def __init__(self, max_workers: int) -> None:
            pools.append(max_workers)
            super().__init__(max_workers=max_workers)

    monkeypatch.setattr(_shipped, "ProcessPoolExecutor", Pool)
    assert (
        cli.main(["ship", *names, "--pin-data", str(root), "--check", "-j", "2"]) == 0
    ), capsys.readouterr()
    assert pools == [2]


@pytest.mark.parametrize("jobs", ["1", "2"])
def test_ship_check_reports_each_failed_build_and_goes_on(
    jobs: str, tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    names = sorted(REFERENCES.values())[:2]
    assert cli.main(["ship", *names, "--pin-data", str(tmp_path), "--check", "-j", jobs]) == 1
    err = capsys.readouterr().err.splitlines()
    assert len(err) == len(names), err
    assert all("pin_data" in line for line in err), err


def test_build_each_gives_the_exception_itself_in_process_and_a_copy_from_a_pool(
    tmp_path: Path,
) -> None:
    names = sorted(REFERENCES.values())[:2]
    (alone,) = _shipped.build_each(names[:1], pin_data=tmp_path, jobs=1)
    assert isinstance(alone, MissingSource)
    pooled = _shipped.build_each(names, pin_data=tmp_path, jobs=2)
    assert [type(one) for one in pooled] == [DescriptionError, DescriptionError]
    assert str(pooled[0]) == str(alone)
