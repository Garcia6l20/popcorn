import os
import re
import shutil
import subprocess
from pathlib import Path

from pcons import find_c_toolchain, get_variant

POPCORN_VERSION = "0.1.0.dev0"
VARIANTS = ("debug", "release", "asan")
GCC_MINIMUM = (16, 1)

project_dir = Path(__file__).resolve().parent.parent
build_dir = Path(os.environ.get("PCONS_BUILD_DIR", project_dir / "build")).resolve()

VARIANT = get_variant("release")
if VARIANT not in VARIANTS:
    raise SystemExit(f"popcorn: unknown variant {VARIANT!r}, expected one of {', '.join(VARIANTS)}")


def _require_tool(name: str, purpose: str) -> str:
    path = shutil.which(name)
    if path is None:
        raise SystemExit(f"popcorn needs {name} on PATH for {purpose}, found none")
    return path


def _gcc_version(gxx: str) -> tuple[int, ...]:
    output = subprocess.run(
        [gxx, "-dumpfullversion"], capture_output=True, text=True, check=False
    ).stdout.strip()
    if not re.fullmatch(r"\d+(\.\d+)*", output):
        return ()
    return tuple(int(part) for part in output.split("."))


def _check_gcc() -> None:
    minimum = ".".join(map(str, GCC_MINIMUM))
    gxx = shutil.which("g++")
    if gxx is None:
        raise SystemExit(f"popcorn needs g++ >= {minimum} as g++, found none")
    version = _gcc_version(gxx)
    if version[:2] < GCC_MINIMUM:
        found = ".".join(map(str, version)) or "an unknown version"
        raise SystemExit(f"popcorn needs g++ >= {minimum} as g++, found {found} at {gxx}")


_check_gcc()
_require_tool("cmake", "building Unicorn")
_require_tool("ninja", "running the build")

toolchain = find_c_toolchain(prefer=["gcc"])
if toolchain.name != "gcc":
    raise SystemExit(f"popcorn needs the gcc toolchain, pcons selected {toolchain.name}")
