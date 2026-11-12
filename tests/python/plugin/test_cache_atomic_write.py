"""A cache entry write that fails leaves no part file behind."""

from __future__ import annotations

from collections.abc import Callable
from pathlib import Path
from typing import cast

import pytest
from popcorn import Snapshot
from popcorn.pytest._cache import Entry


class SavedSnapshot:
    def save(self, path: Path) -> None:
        path.write_bytes(b"snap")


def entry_in(directory: Path) -> Entry:
    return Entry.of(directory, "key")


def save(entry: Entry) -> None:
    entry.save(cast(Snapshot, SavedSnapshot()), None, b"coverage", {})


def fail_after_a_partial_write(error: BaseException | type[BaseException]) -> Callable[..., int]:
    def write_bytes(self: Path, data: bytes) -> int:
        with self.open("wb") as out:
            out.write(data[:1])
        raise error

    return write_bytes


def fail_replace(error: BaseException | type[BaseException]) -> Callable[..., Path]:
    def replace(*args: object, **kwargs: object) -> Path:
        raise error

    return replace


@pytest.mark.parametrize("error", [OSError, KeyboardInterrupt])
def test_a_failed_replace_leaves_no_part_file_in_the_cache_entry(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, error: type[BaseException]
) -> None:
    monkeypatch.setattr(Path, "replace", fail_replace(error))
    with pytest.raises(error):
        save(entry_in(tmp_path))
    assert list(tmp_path.iterdir()) == []


@pytest.mark.parametrize("error", [OSError, KeyboardInterrupt])
def test_a_failed_write_leaves_no_part_file_in_the_cache_entry(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, error: type[BaseException]
) -> None:
    monkeypatch.setattr(Path, "write_bytes", fail_after_a_partial_write(error))
    with pytest.raises(error):
        save(entry_in(tmp_path))
    assert list(tmp_path.iterdir()) == []


def test_a_good_entry_write_leaves_exactly_its_files(tmp_path: Path) -> None:
    entry = entry_in(tmp_path)
    save(entry)
    assert sorted(path.name for path in tmp_path.iterdir()) == ["key.json", "key.pcov", "key.snap"]


def test_a_failed_cleanup_does_not_hide_the_cache_entry_write_error(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    unlink_path = Path.unlink

    def unlink(self: Path, missing_ok: bool = False) -> None:
        if ".tmp." in self.name:
            raise PermissionError("unlink refused")
        unlink_path(self, missing_ok=missing_ok)

    monkeypatch.setattr(Path, "replace", fail_replace(OSError("disk full")))
    monkeypatch.setattr(Path, "unlink", unlink)
    with pytest.raises(OSError, match="disk full"):
        save(entry_in(tmp_path))
