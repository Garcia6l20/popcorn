from pathlib import Path

from pcons import context

from popcorn_build.config import project_dir
from popcorn_build.firmware import firmware
from popcorn_build.testing import add_doctest

project = context.current_project
here = Path(__file__).resolve().parent
core = project.get_target("popcorn_core")
obs = project.get_target("popcorn_obs")
obs_sources = str(project_dir / "native" / "src" / "obs")

test_thumb = add_doctest("obs-thumb", [here / "test_thumb.cpp"], [obs, core])
test_thumb.private.include_dirs.append(obs_sources)

branches_elf, branches_firmware = firmware("core", "branches")
test_ranges = add_doctest(
    "obs-ranges",
    [here / "test_ranges.cpp"],
    [obs, core],
    env={"POPCORN_FW_BRANCHES": str(branches_elf)},
)
if branches_firmware is not None:
    test_ranges.depends(branches_firmware)
