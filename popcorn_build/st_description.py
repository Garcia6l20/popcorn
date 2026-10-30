import os
import subprocess
import sys
from pathlib import Path

from popcorn_build.cmsis_dirs import VARIABLE, cmsis_device_dirs, with_checkout_dirs
from popcorn_build.config import build_dir, project_dir

_PROBE = """
import sys
import types

package = types.ModuleType("popcorn")
package.__path__ = [sys.argv[1]]
sys.modules["popcorn"] = package

from popcorn.stm32.description import DescriptionError, description_path

try:
    print(description_path(sys.argv[2]))
except DescriptionError as exc:
    print(exc, file=sys.stderr)
    sys.exit(1)
"""


def probe_env() -> dict[str, str]:
    """The environment of the description probe: this one, with `POPCORN_CMSIS_DEVICE_DIRS`
    naming the CMSIS device directories of the build directory first."""
    return os.environ | {
        VARIABLE: with_checkout_dirs(
            os.environ.get(VARIABLE), cmsis_device_dirs(project_dir, build_dir)
        )
    }


def cached_st_description(part: str, variable: str) -> Path | None:
    """Path of the description of @p part, from @p variable when set, else by the lookup of
    `mcu()`: a description directory, the shipped layers, then a build from ST's files.

    The build runs popcorn's description builder in a child interpreter from the source tree, so
    it needs neither the native extension nor a staged package. The child gets
    `POPCORN_CMSIS_DEVICE_DIRS` with the CMSIS device directories this build fetched first, as
    the description tests do, so it takes the IRQ names from the device header. Returns None,
    after a warning showing why, when the lookup fails, as it does for an unshipped part without
    ST's files.
    """
    preset = os.environ.get(variable)
    if preset:
        return Path(preset)
    probe = subprocess.run(
        [sys.executable, "-c", _PROBE, str(project_dir / "python" / "popcorn"), part],
        capture_output=True,
        text=True,
        check=False,
        env=probe_env(),
    )
    if probe.returncode != 0:
        print(
            f"popcorn: warning: {part} tests not registered, the description build failed:\n"
            f"{probe.stderr.strip()}",
            file=sys.stderr,
        )
        return None
    path = probe.stdout.strip()
    if not path:
        raise SystemExit(f"popcorn: the {part} description build printed no path")
    return Path(path)
