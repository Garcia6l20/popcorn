"""Plugin builds that several test modules share inside one pytest run."""

from __future__ import annotations

from pathlib import Path

import pytest


@pytest.fixture(scope="session")
def shared_cache(run_directory: Path) -> Path:
    """A plugin cache for tests that need a built fixture plugin and test nothing of its build.
    The cache's own lock makes the workers build each entry once. A test that edits a plugin
    source, a helper or the cache takes a cache of its own instead."""
    cache = run_directory / "plugin-cache"
    cache.mkdir(exist_ok=True)
    return cache
