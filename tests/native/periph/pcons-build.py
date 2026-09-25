from pathlib import Path

from pcons import context

from popcorn_build.config import project_dir
from popcorn_build.firmware import firmware
from popcorn_build.testing import add_doctest

here = Path(__file__).resolve().parent
project = context.current_project
core = project.get_target("popcorn_core")
test_support = project.get_target("popcorn-test-support")
toy = str(project_dir / "tests" / "data" / "desc" / "toy.json")

add_doctest(
    "periph-interfaces",
    [here / "test_interfaces.cpp", here / "test_lines.cpp", here / "test_register_file.cpp"],
    [core],
    env={"POPCORN_TOY_DESC": toy},
)
add_doctest("periph-register-layer", [here / "test_register_layer.cpp"], [core])
calls_elf, calls_firmware = firmware("core", "calls")
test_registry = add_doctest(
    "periph-registry",
    [here / "test_registry.cpp"],
    [test_support, core],
    env={
        "POPCORN_M33_DESC": str(project_dir / "tests" / "data" / "core" / "m33.json"),
        "POPCORN_FW_CALLS": str(calls_elf),
    },
)
if calls_firmware is not None:
    test_registry.depends(calls_firmware)

periph_elf, periph_firmware = firmware("core", "periph")
test_mmio = add_doctest(
    "periph-mmio",
    [here / "test_mmio_dispatch.cpp"],
    [test_support, core],
    env={
        "POPCORN_M33_DESC": str(project_dir / "tests" / "data" / "core" / "m33.json"),
        "POPCORN_FW_PERIPH": str(periph_elf),
    },
)
if periph_firmware is not None:
    test_mmio.depends(periph_firmware)
