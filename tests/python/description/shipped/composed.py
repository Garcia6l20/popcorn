"""The composition of a shipped part, shared by the workers of one pytest run.

`composed` composes a part once per run and keeps its text, compressed, under the run's shared
directory. A worker that needs a part another one composed already reads it instead.
"""

from __future__ import annotations

import fcntl
import zlib
from pathlib import Path

from popcorn.stm32.description._shipped import compose_shipped

_directory: Path | None = None


def share(directory: Path | None) -> None:
    """Keep the compositions under @p directory, None composes every call."""
    global _directory
    _directory = directory


def composed(name: str) -> str:
    """The text `compose_shipped` returns for the shipped part @p name from SHIPPED_DIR."""
    if _directory is None:
        return compose_shipped(name)
    stored = _directory / f"{name}.z"
    if stored.exists():
        return zlib.decompress(stored.read_bytes()).decode()
    with (_directory / f"{name}.lock").open("a") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        if stored.exists():
            return zlib.decompress(stored.read_bytes()).decode()
        text = compose_shipped(name)
        partial = stored.with_suffix(".partial")
        partial.write_bytes(zlib.compress(text.encode(), 1))
        partial.rename(stored)
        return text
