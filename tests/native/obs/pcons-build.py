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

test_support = project.get_target("popcorn-test-support")
core_data = project_dir / "tests" / "data" / "core"
tb_firmware = {("obs", "tb"): firmware("obs", "tb")} | {
    ("core", name): firmware("core", name) for name in ("branches", "alu", "fnv")
}
test_tb_ranges = add_doctest(
    "obs-tb-ranges",
    [here / "test_tb_ranges.cpp"],
    [obs, test_support, core],
    env={
        "POPCORN_M33_DESC": str(core_data / "m33.json"),
        "POPCORN_M33_CODE_ALIAS_DESC": str(core_data / "m33_code_alias.json"),
        **{f"POPCORN_FW_{name.upper()}": str(elf) for (_, name), (elf, _) in tb_firmware.items()},
    },
)
test_tb_ranges.private.include_dirs.append(obs_sources)
for _, target in tb_firmware.values():
    if target is not None:
        test_tb_ranges.depends(target)
