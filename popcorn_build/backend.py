"""PEP 517 backend that gives `pcons.pyproject` a version computed from git.

`pcons.pyproject` reads a static `[project].version`. This wrapper replaces its pyproject loader
with one that fills in `popcorn_build.version.popcorn_version` when `version` is listed in
`dynamic`, and drops that entry, then re-exports every hook. The patch of
`pcons.pyproject._load_pyproject` is the only coupling to pcons internals.
"""

from __future__ import annotations

import functools
from pathlib import Path
from typing import Any

import pcons.pyproject as _pcons
from pcons.pyproject import (
    build_editable,
    build_sdist,
    build_wheel,
    get_requires_for_build_editable,
    get_requires_for_build_sdist,
    get_requires_for_build_wheel,
    prepare_metadata_for_build_editable,
    prepare_metadata_for_build_wheel,
)

from popcorn_build.version import PROJECT_DIR, popcorn_version

__all__ = [
    "build_editable",
    "build_sdist",
    "build_wheel",
    "get_requires_for_build_editable",
    "get_requires_for_build_sdist",
    "get_requires_for_build_wheel",
    "prepare_metadata_for_build_editable",
    "prepare_metadata_for_build_wheel",
]

_load_pyproject = _pcons._load_pyproject  # pyright: ignore[reportPrivateUsage]


@functools.cache
def _version(source_dir: Path) -> str:
    return popcorn_version(source_dir)


def _load_with_version(source_dir: Path) -> dict[str, Any]:
    table = _load_pyproject(source_dir)
    project = table.get("project", {})
    dynamic: list[str] = project.get("dynamic", [])
    if "version" in dynamic and source_dir.resolve() == PROJECT_DIR:
        project["version"] = _version(PROJECT_DIR)
        project["dynamic"] = [name for name in dynamic if name != "version"]
    return table


_pcons._load_pyproject = _load_with_version  # pyright: ignore[reportPrivateUsage]
