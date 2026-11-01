"""The m33 variant generator, its build-rule names and the per-user Python cache."""

from __future__ import annotations

import os
import runpy
import subprocess
import sys
import tempfile
from collections.abc import Iterator
from pathlib import Path
from typing import cast

import derived_data
import pytest
from derived_data import BASE, GENERATOR, DerivedDataError

from popcorn_build.testing import M33_VARIANT_NAMES


def generator_variants() -> set[str]:
    return set(cast(dict[str, object], runpy.run_path(str(GENERATOR))["VARIANTS"]))


@pytest.fixture
def temp_root(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> Iterator[Path]:
    monkeypatch.setattr(tempfile, "tempdir", str(tmp_path))
    derived_data.variants_dir.cache_clear()
    yield tmp_path
    derived_data.variants_dir.cache_clear()


def test_the_build_rule_declares_every_variant_the_generator_derives() -> None:
    assert generator_variants() == set(M33_VARIANT_NAMES)


def test_the_generator_writes_exactly_the_names_it_is_given(tmp_path: Path) -> None:
    command = [sys.executable, str(GENERATOR), str(BASE), str(tmp_path), *M33_VARIANT_NAMES]
    subprocess.run(command, check=True)
    assert sorted(path.stem for path in tmp_path.iterdir()) == sorted(M33_VARIANT_NAMES)


def test_the_generator_refuses_an_unknown_name(tmp_path: Path) -> None:
    command = [sys.executable, str(GENERATOR), str(BASE), str(tmp_path), "m33_missing"]
    refused = subprocess.run(command, capture_output=True, text=True)
    assert refused.returncode != 0 and "KeyError: 'm33_missing'" in refused.stderr


def test_the_cache_is_private_to_the_user(temp_root: Path) -> None:
    written = derived_data.model_json()
    cache = temp_root / f"popcorn-test-data-{os.getuid()}"
    assert written.parent.parent == cache and written.is_file()
    for directory in (cache, written.parent):
        assert directory.stat().st_mode & 0o077 == 0


def test_an_unwritable_cache_raises_one_named_error_at_use(temp_root: Path) -> None:
    cache = derived_data.cache_root()
    cache.mkdir(mode=0o700)
    cache.chmod(0)
    try:
        with pytest.raises(DerivedDataError, match="cannot write the m33 test variants"):
            derived_data.model_json()
    finally:
        cache.chmod(0o700)


def test_a_cache_of_another_user_is_refused(
    temp_root: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    owner = os.getuid()
    (temp_root / f"popcorn-test-data-{owner + 1}").mkdir(mode=0o700)
    monkeypatch.setattr(os, "getuid", lambda: owner + 1)
    with pytest.raises(DerivedDataError, match=f"belongs to uid {owner},"):
        derived_data.model_json()
