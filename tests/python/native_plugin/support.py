"""Fixture plugin directories copied to a test's temporary directory, and subprocess runners."""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
import textwrap
from pathlib import Path

FIXTURES = Path(__file__).resolve().parent / "fixtures"


def copied(name: str, into: Path) -> Path:
    """A copy of the fixture plugin @p name under @p into, a fresh path the test may edit."""
    target = into / "src" / name
    shutil.copytree(FIXTURES / name, target)
    return target


def entries(cache: Path, name: str) -> list[Path]:
    """The cache entry directories of @p name under @p cache."""
    return sorted(path for path in cache.glob(f"{name}-*") if path.is_dir())


def run_python(
    code: str, cache: Path, extra: dict[str, str] | None = None
) -> subprocess.CompletedProcess[str]:
    """Run @p code in a fresh interpreter with @p cache as POPCORN_PLUGIN_CACHE."""
    environment = {**os.environ, "POPCORN_PLUGIN_CACHE": str(cache), **(extra or {})}
    return subprocess.run(
        [sys.executable, "-c", textwrap.dedent(code)],
        capture_output=True,
        text=True,
        env=environment,
        check=False,
        timeout=240,
    )
