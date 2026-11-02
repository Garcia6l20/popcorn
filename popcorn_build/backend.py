"""PEP 517 backend that gives `pcons.pyproject` a version computed from git.

`pcons.pyproject` reads a static `[project].version`. This wrapper replaces its pyproject loader
with one that fills in `popcorn_build.version.popcorn_version` when `version` is listed in
`dynamic`, and drops that entry, then re-exports every hook. `build_editable` also leaves out the
`[tool.pcons] variables`, so the editable install keeps the development build of `build/`. The
patch of `pcons.pyproject._load_pyproject` is the only coupling to pcons internals.
"""

from __future__ import annotations

import functools
from pathlib import Path
from typing import Any

import pcons.pyproject as _pcons
from pcons.pyproject import (
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
_editable = False


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
    if _editable:
        table.get("tool", {}).get("pcons", {}).pop("variables", None)
    return table


_pcons._load_pyproject = _load_with_version  # pyright: ignore[reportPrivateUsage]


def build_editable(
    wheel_directory: str,
    config_settings: dict[str, Any] | None = None,
    metadata_directory: str | None = None,
) -> str:
    """`pcons.pyproject.build_editable` with the development build of `build/`.

    The editable install builds in `build/`, the directory of the development build. The
    `[tool.pcons] variables` describe the wheel, so they are left out here and `build/` keeps its
    tests, examples and their targets.
    """
    global _editable
    _editable = True
    try:
        return _pcons.build_editable(wheel_directory, config_settings, metadata_directory)
    finally:
        _editable = False
