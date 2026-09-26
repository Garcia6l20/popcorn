"""gcc-ar lookup for LTO archives, a workaround: remove once pcons resolves gcc-ar itself."""

import os
from pathlib import Path


def _sibling(gxx: Path) -> Path | None:
    stem, found, suffix = gxx.name.rpartition("g++")
    if not found:
        return None
    archiver = gxx.with_name(f"{stem}gcc-ar{suffix}")
    return archiver if archiver.is_file() else None


def gcc_ar_for(gxx: Path, search_path: str | None = None) -> Path:
    """The gcc-ar beside @p gxx, whose LTO plugin matches that compiler.

    The trailing `g++` of the basename becomes `gcc-ar`, keeping any target prefix and version
    suffix: `g++-16` gives `gcc-ar-16`, `x86_64-pc-linux-gnu-g++` gives
    `x86_64-pc-linux-gnu-gcc-ar`. When @p gxx has no such sibling, as a ccache wrapper directory
    does not, the resolved symlink is tried, then the next directory of @p search_path, PATH by
    default, holding the same compiler name: the compiler the wrapper runs.
    """
    if "g++" not in gxx.name:
        raise SystemExit(f"popcorn: cannot derive gcc-ar from {gxx}, its name has no g++")
    candidates = [gxx, gxx.resolve()]
    directories = (search_path if search_path is not None else os.environ.get("PATH", "")).split(
        os.pathsep
    )
    wrapper = gxx.parent.resolve()
    after = [Path(entry) for entry in directories if entry]
    seen = [index for index, entry in enumerate(after) if entry.resolve() == wrapper]
    if seen:
        after = after[seen[0] + 1 :]
    candidates += [entry / gxx.name for entry in after if (entry / gxx.name).is_file()]
    for candidate in candidates:
        if (archiver := _sibling(candidate)) is not None:
            return archiver
    stem, _, suffix = gxx.name.rpartition("g++")
    raise SystemExit(
        f"popcorn needs {gxx.with_name(f'{stem}gcc-ar{suffix}')} beside {gxx} for LTO archives,"
        " found none, nor beside the compiler it resolves to"
    )
