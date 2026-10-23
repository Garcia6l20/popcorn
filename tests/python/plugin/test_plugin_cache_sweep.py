"""The session-end sweep of the on-disk boot snapshot cache."""

from __future__ import annotations

import fcntl
import json
import os
import shutil
import time
from pathlib import Path
from typing import cast

import pytest
from popcorn.pytest._cache import Entry, sweep

from .helpers import write_conftest, write_project

pytest_plugins = ["pytester"]

BASE = ("-p", "no:asyncio")
DAY = 86400.0
TEST = "def test_boot(machine):\n    pass\n"


def run(pytester: pytest.Pytester, *args: str) -> pytest.RunResult:
    return pytester.runpytest(*BASE, *args)


def boot_directory(pytester: pytest.Pytester) -> Path:
    return pytester.path / ".pytest_cache" / "d" / "popcorn" / "boot"


def booted(pytester: pytest.Pytester) -> Entry:
    write_conftest(pytester)
    write_project(pytester)
    pytester.makepyfile(TEST)
    run(pytester).assert_outcomes(passed=1)
    (info,) = boot_directory(pytester).glob("*.json")
    return Entry.of(info.parent, info.stem)


def rewrite(entry: Entry, **inputs: object) -> None:
    data = cast(dict[str, object], json.loads(entry.info.read_text()))
    cast(dict[str, object], data["inputs"]).update(inputs)
    entry.info.write_text(json.dumps(data))


def age(entry: Entry, days: float) -> None:
    then = time.time() - days * DAY
    os.utime(entry.info, (then, then))


def survives(entry: Entry) -> bool:
    return entry.info.exists() and entry.snapshot.exists()


def test_a_live_entry_is_kept(pytester: pytest.Pytester) -> None:
    entry = booted(pytester)
    run(pytester).assert_outcomes(passed=1)
    assert survives(entry)


def test_the_entry_records_its_inputs(pytester: pytest.Pytester) -> None:
    entry = booted(pytester)
    inputs = json.loads(entry.info.read_text())["inputs"]
    assert set(inputs) == {"version", "native", "images"}
    ((path, digest),) = inputs["images"]
    assert Path(path).is_file() and len(digest) == 64


def test_a_stale_native_digest_deletes_the_entry(pytester: pytest.Pytester) -> None:
    entry = booted(pytester)
    rewrite(entry, native="0" * 64)
    run(pytester).assert_outcomes(passed=1)
    assert not entry.info.exists()
    assert not entry.snapshot.exists()
    assert not entry.lock.exists()


def test_a_stale_version_deletes_the_entry(pytester: pytest.Pytester) -> None:
    entry = booted(pytester)
    rewrite(entry, version="0.0.0-old")
    run(pytester).assert_outcomes(passed=1)
    assert not survives(entry)


def test_a_stale_image_digest_deletes_the_entry(pytester: pytest.Pytester) -> None:
    entry = booted(pytester)
    path = json.loads(entry.info.read_text())["inputs"]["images"][0][0]
    rewrite(entry, images=[[path, "f" * 64]])
    run(pytester).assert_outcomes(passed=1)
    assert not survives(entry)


def test_a_missing_image_path_deletes_the_entry(pytester: pytest.Pytester) -> None:
    entry = booted(pytester)
    digest = json.loads(entry.info.read_text())["inputs"]["images"][0][1]
    rewrite(entry, images=[[str(pytester.path / "gone.elf"), digest]])
    run(pytester).assert_outcomes(passed=1)
    assert not survives(entry)


def test_an_entry_of_the_old_format_is_deleted(pytester: pytest.Pytester) -> None:
    entry = booted(pytester)
    old = Entry.of(entry.info.parent, "0" * 64)
    shutil.copyfile(entry.snapshot, old.snapshot)
    old.info.write_text(json.dumps({"stop": None}))
    run(pytester).assert_outcomes(passed=1)
    assert not old.info.exists()
    assert not old.snapshot.exists()
    assert survives(entry)


def test_an_unused_entry_expires_with_the_ini_option(pytester: pytest.Pytester) -> None:
    entry = booted(pytester)
    stale = Entry.of(entry.info.parent, "1" * 64)
    shutil.copyfile(entry.snapshot, stale.snapshot)
    shutil.copyfile(entry.info, stale.info)
    age(stale, 3)
    run(pytester, "-o", "popcorn_cache_max_age_days=5").assert_outcomes(passed=1)
    assert survives(stale)
    run(pytester, "-o", "popcorn_cache_max_age_days=2").assert_outcomes(passed=1)
    assert not survives(stale)
    assert survives(entry)


def test_the_default_age_limit_is_seven_days(pytester: pytest.Pytester) -> None:
    entry = booted(pytester)
    stale = Entry.of(entry.info.parent, "2" * 64)
    shutil.copyfile(entry.snapshot, stale.snapshot)
    shutil.copyfile(entry.info, stale.info)
    age(stale, 6)
    run(pytester).assert_outcomes(passed=1)
    assert survives(stale)
    age(stale, 8)
    run(pytester).assert_outcomes(passed=1)
    assert not survives(stale)


def test_a_zero_age_limit_keeps_old_entries(pytester: pytest.Pytester) -> None:
    entry = booted(pytester)
    stale = Entry.of(entry.info.parent, "3" * 64)
    shutil.copyfile(entry.snapshot, stale.snapshot)
    shutil.copyfile(entry.info, stale.info)
    age(stale, 400)
    run(pytester, "-o", "popcorn_cache_max_age_days=0").assert_outcomes(passed=1)
    assert survives(stale)


def test_a_bad_age_limit_is_a_usage_error(pytester: pytest.Pytester) -> None:
    write_conftest(pytester)
    result = run(pytester, "-o", "popcorn_cache_max_age_days=soon")
    assert result.ret == pytest.ExitCode.USAGE_ERROR
    result.stderr.fnmatch_lines(["*popcorn_cache_max_age_days must be*"])


def test_a_hit_touches_the_entry(pytester: pytest.Pytester) -> None:
    entry = booted(pytester)
    age(entry, 5)
    before = entry.info.stat().st_mtime
    run(pytester, "-o", "popcorn_cache_max_age_days=2").assert_outcomes(passed=1)
    assert survives(entry)
    assert entry.info.stat().st_mtime > before + 4 * DAY


def test_a_locked_entry_is_skipped(pytester: pytest.Pytester) -> None:
    entry = booted(pytester)
    rewrite(entry, native="0" * 64)
    descriptor = os.open(entry.lock, os.O_RDWR | os.O_CREAT)
    try:
        fcntl.flock(descriptor, fcntl.LOCK_EX)
        assert sweep(entry.info.parent, 7) == 0
        assert survives(entry)
    finally:
        os.close(descriptor)
    assert sweep(entry.info.parent, 7) == 1
    assert not survives(entry)


def test_an_xdist_worker_does_not_sweep(pytester: pytest.Pytester) -> None:
    entry = booted(pytester)
    rewrite(entry, native="0" * 64)
    conftest = pytester.path / "conftest.py"
    conftest.write_text(
        conftest.read_text() + "\n\ndef pytest_configure(config):\n    config.workerinput = {}\n"
    )
    run(pytester)
    assert survives(entry)


def test_the_controller_sweeps_under_xdist(pytester: pytest.Pytester) -> None:
    pytest.importorskip("xdist")
    entry = booted(pytester)
    rewrite(entry, native="0" * 64)
    result = pytester.runpytest_subprocess(*BASE, "-n", "2", timeout=60)
    result.assert_outcomes(passed=1)
    assert not survives(entry)
