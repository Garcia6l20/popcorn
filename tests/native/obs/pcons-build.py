from pathlib import Path

from pcons import context

from popcorn_build.config import build_dir, project_dir
from popcorn_build.firmware import firmware
from popcorn_build.testing import add_doctest, alias_bare_name

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

lines_elf, lines_firmware = firmware("obs", "lines")
cut_elf, cut_firmware = firmware("obs", "cut")
test_lcov = add_doctest(
    "obs-lcov",
    [here / "test_lcov.cpp"],
    [obs, test_support, core],
    env={
        "POPCORN_M33_DESC": str(core_data / "m33.json"),
        "POPCORN_FW_LINES": str(lines_elf),
        "POPCORN_FW_CUT": str(cut_elf),
        "POPCORN_PROJECT_DIR": str(project_dir),
        "POPCORN_OBS_SCRATCH": str(build_dir / "test-obs-scratch"),
    },
)
test_lcov.private.include_dirs.append(obs_sources)
for target in (lines_firmware, cut_firmware):
    if target is not None:
        test_lcov.depends(target)

test_pcov = add_doctest(
    "obs-pcov",
    [here / "test_pcov.cpp"],
    [obs, test_support, core],
    env={
        "POPCORN_M33_DESC": str(core_data / "m33.json"),
        "POPCORN_FW_BRANCHES": str(branches_elf),
        "POPCORN_OBS_SCRATCH": str(build_dir / "test-obs-scratch"),
    },
)
if branches_firmware is not None:
    test_pcov.depends(branches_firmware)

test_drcov = add_doctest(
    "obs-drcov",
    [here / "test_drcov.cpp"],
    [obs, test_support, core],
    env={
        "POPCORN_M33_DESC": str(core_data / "m33.json"),
        "POPCORN_FW_LINES": str(lines_elf),
    },
)
if lines_firmware is not None:
    test_drcov.depends(lines_firmware)

periph_elf, periph_firmware = firmware("core", "periph")
test_pctr = add_doctest(
    "obs-pctr",
    [here / "test_pctr.cpp"],
    [obs, test_support, core],
    env={
        "POPCORN_M33_DESC": str(core_data / "m33.json"),
        "POPCORN_FW_PERIPH": str(periph_elf),
        "POPCORN_FW_ALU": str(tb_firmware[("core", "alu")][0]),
        "POPCORN_OBS_SCRATCH": str(build_dir / "test-obs-scratch"),
    },
)
for target in (periph_firmware, tb_firmware[("core", "alu")][1]):
    if target is not None:
        test_pctr.depends(target)

obs_probe = project.Program(
    "obs-probe", project.default_environment, sources=[here / "obs_probe.cpp"]
)
for dep in (obs, test_support, core):
    obs_probe.link(dep)
alias_bare_name(obs_probe)
