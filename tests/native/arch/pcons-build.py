from pathlib import Path

from pcons import context

from popcorn_build.config import project_dir
from popcorn_build.firmware import firmware
from popcorn_build.testing import add_doctest

here = Path(__file__).resolve().parent
project = context.current_project
core = project.get_target("popcorn_core")
test_support = project.get_target("popcorn-test-support")
core_data = project_dir / "tests" / "data" / "core"

add_doctest("arch-interface", [here / "test_interface.cpp"], [core])

arch_firmware = {name: firmware("arch", name) for name in ("scs",)}
arch_env = {
    "POPCORN_M33_DESC": str(core_data / "m33.json"),
    **{f"POPCORN_FW_{name.upper()}": str(elf) for name, (elf, _) in arch_firmware.items()},
}

test_scs = add_doctest("arch-scs", [here / "test_scs.cpp"], [test_support, core], env=arch_env)
test_scs.private.include_dirs.append(project_dir / "native" / "src" / "arch" / "armv8m")
for _, target in arch_firmware.values():
    if target is not None:
        test_scs.depends(target)
