import shutil
import tomllib
from collections.abc import Callable
from pathlib import Path

DIRTY_SUFFIX = "-dirty"


def package_version(commit: str, digest: str, dirty: bool) -> str:
    """
    @brief Version recorded in the package file of a built submodule.
    @param commit Submodule commit.
    @param digest Digest of the build options.
    @param dirty True when the submodule tree has local changes.
    @return `<commit>+<digest>`, with a `-dirty` suffix for a dirty tree so that no clean
            build matches it.
    """
    version = f"{commit}+{digest}"
    return version + DIRTY_SUFFIX if dirty else version


def resolve(
    pkg_file: Path,
    build_dir: Path,
    version: str,
    dirty: bool,
    library: str,
    build: Callable[[str], Path],
) -> Path:
    """
    @brief Decide whether a submodule package is reused or rebuilt, and return its prefix.
    @param pkg_file Package file written by the previous build.
    @param build_dir CMake build directory of the package.
    @param version Result of package_version() for the current tree.
    @param dirty True when the submodule tree has local changes.
    @param library Library file expected under `lib` of the prefix.
    @param build Builds the package at the given version and returns its prefix.
    @return The package prefix.

    A clean tree reuses a clean build of the same version. A clean tree that finds a dirty
    record removes the CMake build directory first, so objects whose restored sources carry
    old modification times are rebuilt. A removal failure raises OSError before any build.
    """
    recorded = tomllib.loads(pkg_file.read_text())["package"] if pkg_file.exists() else None
    if not dirty and recorded is not None:
        if recorded["version"] == version and (Path(recorded["prefix"]) / "lib" / library).exists():
            return Path(recorded["prefix"])
        if recorded["version"].endswith(DIRTY_SUFFIX) and build_dir.exists():
            shutil.rmtree(build_dir)
    return build(version)
