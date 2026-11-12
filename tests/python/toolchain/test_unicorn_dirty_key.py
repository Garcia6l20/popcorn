from __future__ import annotations

import os
from pathlib import Path

import pytest

from popcorn_build.package_key import package_version, resolve

COMMIT = "0e6243596ff44a6aa816b7aa6de653339b9a6ef0"
DIGEST = "de98f3ad6de2"
LIBRARY = "libunicorn.a"


class Builder:
    def __init__(self, root: Path) -> None:
        self.root = root
        self.pkg_file = root / "unicorn.pcons-pkg.toml"
        self.build_dir = root / "build" / "unicorn"
        self.prefix = root / "install"
        self.calls: list[str] = []
        self.build_dir_present_at_build: list[bool] = []

    def __call__(self, version: str) -> Path:
        self.calls.append(version)
        self.build_dir_present_at_build.append(self.build_dir.exists())
        (self.prefix / "lib").mkdir(parents=True, exist_ok=True)
        (self.prefix / "lib" / LIBRARY).write_bytes(b"")
        self.build_dir.mkdir(parents=True, exist_ok=True)
        self.pkg_file.write_text(
            f'[package]\nname = "unicorn"\nversion = "{version}"\nprefix = "{self.prefix}"\n'
        )
        return self.prefix

    def configure(self, dirty: bool, digest: str = DIGEST) -> Path:
        return resolve(
            self.pkg_file,
            self.build_dir,
            package_version(COMMIT, digest, dirty),
            dirty,
            LIBRARY,
            self,
        )


def test_a_dirty_build_records_a_version_no_clean_build_of_the_same_commit_matches() -> None:
    assert package_version(COMMIT, DIGEST, dirty=True) != package_version(
        COMMIT, DIGEST, dirty=False
    )


def test_a_clean_build_at_the_recorded_clean_version_is_reused(tmp_path: Path) -> None:
    builder = Builder(tmp_path)
    builder.configure(dirty=False)
    builder.configure(dirty=False)
    assert len(builder.calls) == 1


def test_a_clean_tree_after_a_dirty_build_rebuilds_and_a_dirty_tree_always_rebuilds(
    tmp_path: Path,
) -> None:
    builder = Builder(tmp_path)
    counts: list[int] = []
    for dirty in (False, True, True, False, False):
        builder.configure(dirty)
        counts.append(len(builder.calls))
    assert counts == [1, 2, 3, 4, 4]


def test_a_missing_library_file_forces_a_rebuild_of_a_clean_tree(tmp_path: Path) -> None:
    builder = Builder(tmp_path)
    builder.configure(dirty=False)
    (builder.prefix / "lib" / LIBRARY).unlink()
    builder.configure(dirty=False)
    assert len(builder.calls) == 2


def test_a_clean_tree_after_a_dirty_record_removes_the_old_build_directory_first(
    tmp_path: Path,
) -> None:
    builder = Builder(tmp_path)
    builder.configure(dirty=False)
    builder.configure(dirty=True)
    builder.configure(dirty=True)
    builder.configure(dirty=False)
    builder.configure(dirty=False)
    assert builder.build_dir_present_at_build == [False, True, True, False]


@pytest.mark.skipif(os.geteuid() == 0, reason="root removes read-only directories")
def test_a_build_directory_that_cannot_be_removed_stops_the_configure(tmp_path: Path) -> None:
    builder = Builder(tmp_path)
    builder.configure(dirty=False)
    builder.configure(dirty=True)
    locked = builder.build_dir / "objects"
    locked.mkdir()
    (locked / "stale.o").write_bytes(b"")
    locked.chmod(0o500)
    try:
        with pytest.raises(OSError):
            builder.configure(dirty=False)
    finally:
        locked.chmod(0o700)
    assert len(builder.calls) == 2


def test_the_options_digest_still_separates_clean_builds() -> None:
    assert package_version(COMMIT, "aaaaaaaaaaaa", dirty=False) != package_version(
        COMMIT, "bbbbbbbbbbbb", dirty=False
    )
