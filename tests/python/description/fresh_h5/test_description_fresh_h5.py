"""The H5 shipped descriptions against a fresh ship build, and the `ship --check` and
`build_each` cases."""

from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

import pytest
from popcorn.stm32.description import __main__ as cli
from popcorn.stm32.description import _shipped
from popcorn.stm32.description._errors import DescriptionError, MissingSource
from popcorn.stm32.description._sources import pin_data_sources

from ..conftest import require_pin_data
from ..fresh_common import H5_PREFIX, check_fresh, names_of
from ..shipped.test_description_shipped_u575 import REFERENCES


@pytest.mark.parametrize("name", names_of(H5_PREFIX))
def test_the_shipped_description_matches_a_fresh_ship_build(name: str) -> None:
    check_fresh(name)


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
