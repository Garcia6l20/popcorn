import os
import subprocess
import sys
from pathlib import Path

from popcorn_build.config import project_dir

_PROBE = """
import sys
import types

package = types.ModuleType("popcorn")
package.__path__ = [sys.argv[1]]
sys.modules["popcorn"] = package

from popcorn.stm32.description import DescriptionError, cached_description

try:
    print(cached_description(sys.argv[2]))
except DescriptionError as exc:
    print(exc, file=sys.stderr)
    sys.exit(1)
"""


def cached_st_description(part: str, variable: str) -> Path | None:
    """Path of the description of @p part, from @p variable when set, else built from ST's files.

    The build runs popcorn's description builder in a child interpreter from the source tree, so
    it needs neither the native extension nor a staged package. Returns None, after a warning
    showing why, when the builder fails, as it does when ST's files are not installed.
    """
    preset = os.environ.get(variable)
    if preset:
        return Path(preset)
    probe = subprocess.run(
        [sys.executable, "-c", _PROBE, str(project_dir / "python" / "popcorn"), part],
        capture_output=True,
        text=True,
        check=False,
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
