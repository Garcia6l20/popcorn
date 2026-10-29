"""The popcorn version, derived from the git tag by setuptools_scm.

`popcorn_version` is the one source of the version: the wheel and sdist metadata, the SDK header
`POPCORN_VERSION_STRING`, the build info and the build script all read it.

The order of the sources is:

- `SETUPTOOLS_SCM_PRETEND_VERSION`, for builds without git metadata such as the container image.
- git, with the `[tool.setuptools_scm]` table of `pyproject.toml`.
- the `Version` field of the `PKG-INFO` an sdist carries, which setuptools_scm reads when the
  tree has no `.git`.
"""

from __future__ import annotations

from pathlib import Path

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


if __name__ == "__main__":
    print(popcorn_version())
