"""Test descriptors derived from `tests/data/core/m33.json`, written once into a temp cache.

The cache is `popcorn-test-data-<uid>` under the temp directory, private to the user. Its key
directories are named by the SHA-256 of the generator and its base part, so an edit to either
writes a fresh one. A writer fills a private staging directory and renames it into place, so
concurrent xdist workers each see a complete set. Nothing is written at import: the first call
of a reader writes, and raises DerivedDataError when the cache cannot be used.
"""

from __future__ import annotations

import functools
import hashlib
import os
import shutil
import stat
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).parents[2]
CORE_DATA = ROOT / "tests" / "data" / "core"
GENERATOR = CORE_DATA / "m33_variants.py"
BASE = CORE_DATA / "m33.json"


class DerivedDataError(RuntimeError):
    """The variant cache cannot be created, written or trusted."""


def cache_root() -> Path:
    """The per-user cache directory under the current temp directory."""
    return Path(tempfile.gettempdir()) / f"popcorn-test-data-{os.getuid()}"


def _require_private(path: Path) -> None:
    info = path.stat()
    if info.st_uid != os.getuid():
        raise DerivedDataError(f"{path} belongs to uid {info.st_uid}, not to uid {os.getuid()}")
    if stat.S_IMODE(info.st_mode) & 0o022:
        raise DerivedDataError(f"{path} is writable by other users")


def _write(cache: Path, target: Path) -> None:
    staging = Path(tempfile.mkdtemp(prefix=f".{target.name}-", dir=cache))
    try:
        subprocess.run([sys.executable, str(GENERATOR), str(BASE), str(staging)], check=True)
        staging.rename(target)
    except OSError:
        if not target.is_dir():
            raise
    finally:
        shutil.rmtree(staging, ignore_errors=True)


@functools.cache
def variants_dir() -> Path:
    """The key directory holding every variant of `m33_variants.py`, written on first use.

    @throws DerivedDataError The cache or its key directory cannot be created or written, or
        belongs to another user.
    """
    key = hashlib.sha256(GENERATOR.read_bytes() + b"\0" + BASE.read_bytes()).hexdigest()[:16]
    cache = cache_root()
    target = cache / key
    try:
        cache.mkdir(mode=0o700, exist_ok=True)
        _require_private(cache)
        if not target.is_dir():
            _write(cache, target)
        _require_private(target)
    except (OSError, subprocess.CalledProcessError) as error:
        raise DerivedDataError(
            f"cannot write the m33 test variants into {cache}: {error}."
            " Point TMPDIR at a directory this user can write."
        ) from error
    return target


def model_json() -> Path:
    """The part of the Python model tests: m33.json with the TIMER and EEPROM peripherals."""
    return variants_dir() / "model.json"
