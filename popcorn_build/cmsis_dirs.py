"""The CMSIS device `Include` directories this checkout fetched, for the description tools.

`python -m popcorn_build.cmsis_dirs` prints the value of `POPCORN_CMSIS_DEVICE_DIRS` with these
directories first and the entries already set after them, so the checkout's own pins win:

    export POPCORN_CMSIS_DEVICE_DIRS="$(uv run python -m popcorn_build.cmsis_dirs)"

Imports neither pcons nor the build configuration, so tests and the shell can call it.
"""

import os
import sys
import tomllib
from collections.abc import Sequence
from pathlib import Path

VARIABLE = "POPCORN_CMSIS_DEVICE_DIRS"
PREFIX = "cmsis-device-"

project_dir = Path(__file__).resolve().parent.parent


def default_build_dir() -> Path:
    """The build directory, `PCONS_BUILD_DIR` when set, else `build` in the checkout."""
    return Path(os.environ.get("PCONS_BUILD_DIR", project_dir / "build")).resolve()


def _listed(project: Path | None, build: Path | None) -> list[tuple[str, Path]]:
    packages = tomllib.loads(((project or project_dir) / "deps.toml").read_text(encoding="utf-8"))[
        "packages"
    ]
    source = (build or default_build_dir()) / "deps" / "src"
    return [(name, source / name / "Include") for name in packages if name.startswith(PREFIX)]


def cmsis_device_dirs(project: Path | None = None, build: Path | None = None) -> list[Path]:
    """`<build>/deps/src/<name>/Include` of every `deps.toml` package of @p project named
    `cmsis-device-*` whose directory exists, in `deps.toml` order. @p project defaults to this
    checkout, @p build to default_build_dir()."""
    return [path for _, path in _listed(project, build) if path.is_dir()]


def missing_cmsis_packs(project: Path | None = None, build: Path | None = None) -> list[str]:
    """Names of the `cmsis-device-*` packages of @p project's `deps.toml` whose `Include`
    directory @p build lacks, in `deps.toml` order. The configure fetches them only when it
    finds `arm-none-eabi-gcc`."""
    return [name for name, path in _listed(project, build) if not path.is_dir()]


def with_checkout_dirs(current: str | None, dirs: Sequence[Path]) -> str:
    """A `POPCORN_CMSIS_DEVICE_DIRS` value: @p dirs first, then the entries of @p current that
    @p dirs does not hold, in their order."""
    entries = [str(path) for path in dirs]
    for entry in (current or "").split(os.pathsep):
        if entry and entry not in entries:
            entries.append(entry)
    return os.pathsep.join(entries)


def main() -> None:
    """Prints the variable's value with this checkout's directories first, and one line on
    stderr for each listed package the build directory lacks."""
    for name in missing_cmsis_packs():
        print(f"popcorn: {default_build_dir()} lacks {name}, run the configure", file=sys.stderr)
    print(with_checkout_dirs(os.environ.get(VARIABLE), cmsis_device_dirs()))


if __name__ == "__main__":
    main()
