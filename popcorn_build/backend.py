"""PEP 517 backend that gives `pcons.pyproject` a version computed from git.

`pcons.pyproject` reads a static `[project].version`. This wrapper replaces its pyproject loader
with one that fills in `popcorn_build.version.popcorn_version` when `version` is listed in
`dynamic`, and drops that entry, then re-exports every hook. `build_editable` also leaves out the
`[tool.pcons] variables`, so the editable install keeps the development build of `build/`.
`build_sdist` ships the files git tracks, submodules included, so git-ignored paths such as
`local/`, `build-*` and the `.pyi` copies of build outputs never reach the archive. A tree with no
git data keeps the file walk of pcons. The patches of `pcons.pyproject._load_pyproject` and
`_sdist_files` are the only coupling to pcons internals.
"""

from __future__ import annotations

import functools
import subprocess
import sys
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

_sdist_files = _pcons._sdist_files  # pyright: ignore[reportPrivateUsage]


def _git(source_dir: Path, *args: str) -> subprocess.CompletedProcess[bytes]:
    command = ["git", "-C", str(source_dir), *args]
    try:
        return subprocess.run(command, capture_output=True, check=False)
    except OSError as error:
        return subprocess.CompletedProcess(command, 127, b"", str(error).encode())


def _walk(source_dir: Path, build_dir: Path, cause: bytes) -> list[Path]:
    line = cause.decode().strip().splitlines()
    print(
        f"popcorn_build.backend: no git file list ({line[0] if line else 'git failed'}), "
        "the sdist lists every file of the tree",
        file=sys.stderr,
    )
    return _sdist_files(source_dir, build_dir)


def tracked_sdist_files(source_dir: Path, build_dir: Path) -> list[Path]:
    """The files git tracks in `source_dir`, submodules included.

    `source_dir` must be the top of its git work tree. Otherwise, as for an unpacked sdist, the file
    walk of pcons lists the files.

    Raises:
        SystemExit: `.gitmodules` names a submodule that is not checked out.
    """
    top = _git(source_dir, "rev-parse", "--show-toplevel")
    if top.returncode != 0:
        return _walk(source_dir, build_dir, top.stderr)
    if Path(top.stdout.decode().strip()).resolve() != source_dir.resolve():
        return _sdist_files(source_dir, build_dir)
    listing = _git(source_dir, "ls-files", "-z", "--recurse-submodules")
    if listing.returncode != 0:
        return _walk(source_dir, build_dir, listing.stderr)
    names = [name for name in listing.stdout.decode().split("\0") if name]
    if (source_dir / ".gitmodules").is_file() and not _submodules_present(names):
        raise SystemExit("the sdist needs the submodule sources, run: git submodule update --init")
    return sorted(path for name in names if (path := source_dir / name).is_file())


def _submodules_present(names: list[str]) -> bool:
    return "third_party/unicorn/CMakeLists.txt" in names and any(
        name.startswith("third_party/reflex/core/include/") for name in names
    )


_pcons._sdist_files = tracked_sdist_files  # pyright: ignore[reportPrivateUsage]


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
