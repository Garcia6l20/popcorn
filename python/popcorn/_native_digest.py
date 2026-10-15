"""Digest of the native code a session runs: `_core`, the popcorn libraries it loads and the
native devices module `popcorn.devices._native`.

Kept free of pytest: the boot cache uses it, and so does code that runs without the pytest plugin.
"""

from __future__ import annotations

import hashlib
from pathlib import Path

from popcorn import _core


def _core_directory() -> Path | None:
    path = getattr(_core, "__file__", None)
    return Path(path).parent if isinstance(path, str) else None


def native_libraries(directory: Path | None = None) -> list[Path]:
    """The `_core` extension, every `libpopcorn_*.so` in `directory` and the
    `devices/_native` extension below it, sorted by path.

    `directory` defaults to the one `_core` was loaded from. Empty when `_core` has no file.
    These are the files beside `_core`, not necessarily the ones loaded: `_core` finds
    `libpopcorn_core.so` through a `RUNPATH` of `$ORIGIN`, and `LD_LIBRARY_PATH` wins over a
    `RUNPATH`, so a session with `LD_LIBRARY_PATH` naming another directory holding a
    `libpopcorn_core.so` runs that library while the digest hashes the one beside `_core`.
    """
    where = directory if directory is not None else _core_directory()
    if where is None:
        return []
    return sorted(
        [
            *where.glob("_core.*.so"),
            *where.glob("libpopcorn_*.so"),
            *where.glob("devices/_native.*.so"),
        ]
    )


def native_digest(directory: Path | None = None) -> str:
    """SHA-256 over the name and bytes of each of `native_libraries(directory)`, lower-case hex.

    Empty when there is no library to hash.
    """
    libraries = native_libraries(directory)
    if not libraries:
        return ""
    digest = hashlib.sha256()
    for path in libraries:
        digest.update(path.name.encode())
        digest.update(path.read_bytes())
    return digest.hexdigest()
