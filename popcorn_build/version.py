"""The popcorn version, derived from the git tag by setuptools_scm.

`popcorn_version` is the one source of the version: the wheel and sdist metadata, the generated
`popcorn_version_string.hpp` behind `popcorn::core::version()`, `sdk.json` and the build script
all read it. `release_version` is the version field of the plugin ABI tag.

The order of the sources is:

- `SETUPTOOLS_SCM_PRETEND_VERSION`, for builds without git metadata such as the container image.
- git, with the `[tool.setuptools_scm]` table of `pyproject.toml`.
- the `Version` field of the `PKG-INFO` an sdist carries, which setuptools_scm reads when the
  tree has no `.git`.
"""

from __future__ import annotations

import sys
from collections.abc import Sequence
from pathlib import Path

from packaging.version import Version

PROJECT_DIR = Path(__file__).resolve().parent.parent


def popcorn_version(root: Path = PROJECT_DIR) -> str:
    """@p root's version as a PEP 440 string.

    A tagged commit gives the tag, `0.1.0` for `v0.1.0`. Later commits give `<next>.dev<distance>`
    with no local segment, so every wheel is uploadable to an index.

    Only tags of the form `v<version>` count, so release-process tags such as `fold-r12-pre` never
    reach the version.

    @throws LookupError when no source gives a version.
    """
    from setuptools_scm import Configuration, _get_version
    from vcs_versioning._environment import VcsEnvironment

    config = Configuration.from_file(
        root / "pyproject.toml", root=str(root), _env=VcsEnvironment.from_env("SETUPTOOLS_SCM")
    )
    version = _get_version(config, force_write_version_files=False)
    if version is None:
        raise LookupError(f"no version for {root}: set SETUPTOOLS_SCM_PRETEND_VERSION")
    return version


def release_version(version: str) -> str:
    """@p version without its dev segment, `0.4.1` for `0.4.1.dev26`, normalized per PEP 440.

    A dev build and the release it leads to share it, so the plugin ABI tag stays the same across
    the commits between two releases.

    @throws packaging.version.InvalidVersion when @p version is not a PEP 440 version.
    """
    parsed = Version(version)
    if parsed.dev is None:
        return parsed.public
    return parsed.public.removesuffix(f".dev{parsed.dev}")


def version_header(version: str) -> str:
    """The text of `popcorn_version_string.hpp`, which defines `POPCORN_VERSION_STRING`."""
    return f'#pragma once\n\n#define POPCORN_VERSION_STRING "{version}"\n'


def main(argv: Sequence[str]) -> int:
    """Print the version, or with arguments output and version, write the version header.

    The header is rewritten only when its text changes, so `version.cpp` rebuilds only then.
    """
    if not argv:
        print(popcorn_version())
        return 0
    output, version = argv
    text = version_header(version)
    target = Path(output)
    if not target.is_file() or target.read_text() != text:
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(text)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
