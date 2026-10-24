"""The session-end sweep of the on-disk boot snapshot cache."""

from __future__ import annotations

import asyncio
import fcntl
import json
import os
import shutil
import threading
import time
from pathlib import Path
from typing import cast

import pytest
from popcorn.pytest._cache import (  # pyright: ignore[reportPrivateUsage]
    Entry,
    _try_lock,  # pyright: ignore[reportPrivateUsage]
    locked,
    sweep,
)

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
        conftest.read_text()
        + "\n\ndef pytest_configure(config):\n"
        + "    config.workerinput = {}\n    config.workeroutput = {}\n"
    )
    run(pytester).assert_outcomes(passed=1)
    assert survives(entry)


def test_the_controller_sweeps_under_xdist(pytester: pytest.Pytester) -> None:
    pytest.importorskip("xdist")
    entry = booted(pytester)
    rewrite(entry, native="0" * 64)
    result = pytester.runpytest_subprocess(*BASE, "-n", "2", timeout=12)
    result.assert_outcomes(passed=1)
    assert not survives(entry)


def test_a_corrupt_entry_is_deleted(pytester: pytest.Pytester) -> None:
    entry = booted(pytester)
    entry.info.write_text("{not json")
    assert sweep(entry.info.parent, 7) == 1
    assert not survives(entry)


def test_entries_sharing_an_image_go_together(pytester: pytest.Pytester) -> None:
    entry = booted(pytester)
    twin = Entry.of(entry.info.parent, "4" * 64)
    shutil.copyfile(entry.snapshot, twin.snapshot)
    shutil.copyfile(entry.info, twin.info)
    path = json.loads(entry.info.read_text())["inputs"]["images"][0][0]
    for one in (entry, twin):
        rewrite(one, images=[[path, "f" * 64]])
    assert sweep(entry.info.parent, 7) == 2
    assert not survives(entry) and not survives(twin)


def test_orphan_files_expire(pytester: pytest.Pytester) -> None:
    entry = booted(pytester)
    directory = entry.info.parent
    orphans = [directory / f"{'5' * 64}.{suffix}" for suffix in ("snap", "lock", "snap.tmp.1")]
    for orphan in orphans:
        orphan.write_bytes(b"x")
    assert sweep(directory, 7) == 0
    assert all(orphan.exists() for orphan in orphans)
    then = time.time() - 8 * DAY
    for orphan in orphans:
        os.utime(orphan, (then, then))
    assert sweep(directory, 7) == 1
    assert not any(orphan.exists() for orphan in orphans)
    assert survives(entry)


def test_a_session_without_a_cache_directory_creates_none(pytester: pytest.Pytester) -> None:
    write_conftest(pytester)
    pytester.makepyfile("def test_nothing():\n    pass\n")
    run(pytester).assert_outcomes(passed=1)
    assert not (pytester.path / ".pytest_cache" / "d" / "popcorn").exists()


@pytest.mark.skipif(os.geteuid() == 0, reason="root ignores directory permissions")
def test_a_read_only_cache_does_not_fail_the_session(pytester: pytest.Pytester) -> None:
    entry = booted(pytester)
    rewrite(entry, native="0" * 64)
    directory = entry.info.parent
    directory.chmod(0o555)
    try:
        run(pytester).assert_outcomes(passed=1)
        assert sweep(directory, 7) == 0
        assert survives(entry)
    finally:
        directory.chmod(0o755)


def holders(path: Path) -> list[str]:
    target = os.stat(path)
    found: list[str] = []
    for name in os.listdir("/proc/self/fd"):
        try:
            seen = os.stat(f"/proc/self/fd/{name}")
        except OSError:
            continue
        if (seen.st_dev, seen.st_ino) == (target.st_dev, target.st_ino):
            found.append(name)
    return found


def test_a_sweeper_whose_lock_was_removed_backs_off(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    entry = Entry.of(tmp_path, "7" * 64)
    flock = fcntl.flock

    def unlinking(descriptor: int, operation: int) -> None:
        entry.lock.unlink()
        flock(descriptor, operation)

    monkeypatch.setattr(fcntl, "flock", unlinking)
    assert _try_lock(entry.lock) is None


def test_a_removed_orphan_group_leaves_no_lock(tmp_path: Path) -> None:
    orphan = tmp_path / f"{'8' * 64}.snap"
    orphan.write_bytes(b"x")
    then = time.time() - 8 * DAY
    os.utime(orphan, (then, then))
    assert sweep(tmp_path, 7) == 1
    assert list(tmp_path.iterdir()) == []


def test_unrelated_old_files_survive_the_sweep(tmp_path: Path) -> None:
    then = time.time() - 30 * DAY
    stray = [tmp_path / "README", tmp_path / ".gitignore", tmp_path / f"{'9' * 63}.snap"]
    for path in stray:
        path.write_bytes(b"x")
        os.utime(path, (then, then))
    folder = tmp_path / "notes"
    folder.mkdir()
    assert sweep(tmp_path, 7) == 0
    assert all(path.exists() for path in stray) and folder.exists()


@pytest.mark.skipif(not Path("/proc/self/fd").is_dir(), reason="needs /proc/self/fd")
def test_a_waiter_on_a_removed_lock_holds_the_new_file(tmp_path: Path) -> None:
    entry = Entry.of(tmp_path, "6" * 64)
    holder = os.open(entry.lock, os.O_RDWR | os.O_CREAT)
    fcntl.flock(holder, fcntl.LOCK_EX)
    entered = threading.Event()
    release = threading.Event()

    async def wait() -> None:
        async with locked(entry):
            entered.set()
            await asyncio.to_thread(release.wait, 10)

    thread = threading.Thread(target=lambda: asyncio.run(wait()))
    thread.start()
    deadline = time.monotonic() + 10
    while len(holders(entry.lock)) < 2:
        assert thread.is_alive() and time.monotonic() < deadline
        time.sleep(0.01)
    entry.lock.unlink()
    os.close(holder)
    assert entered.wait(5)
    probe = os.open(entry.lock, os.O_RDWR | os.O_CREAT)
    try:
        with pytest.raises(BlockingIOError):
            fcntl.flock(probe, fcntl.LOCK_EX | fcntl.LOCK_NB)
    finally:
        os.close(probe)
        release.set()
        thread.join()
