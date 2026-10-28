from pathlib import Path

from pcons import context

from popcorn_build.config import project_dir
from popcorn_build.firmware import firmware
from popcorn_build.st_description import cached_st_description
from popcorn_build.testing import add_doctest, popcorn_libraries

here = Path(__file__).resolve().parent
project = context.current_project
core = project.get_target("popcorn_core")
core_static = project.get_target("popcorn_core_static")
test_support = project.get_target("popcorn-test-support")
toy = str(project_dir / "tests" / "data" / "desc" / "toy.json")
u575 = cached_st_description("STM32U575QGIx", "POPCORN_U575_DESC")

add_doctest(
    "periph-interfaces",
    [here / "test_interfaces.cpp", here / "test_lines.cpp", here / "test_register_file.cpp"],
    [core],
    env={"POPCORN_TOY_DESC": toy},
)
add_doctest(
    "periph-bus-typeinfo",
    [here / "test_bus_typeinfo.cpp"],
    popcorn_libraries("stm32", "usb", "core"),
)
add_doctest("periph-register-layer", [here / "test_register_layer.cpp"], [core_static])
add_doctest("periph-usb-host", [here / "test_usb_host.cpp"], popcorn_libraries("usb", "core"))
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

test_shared_page = add_doctest(
    "periph-shared-page",
    [here / "test_shared_page.cpp"],
    [test_support, core],
    env={
        "POPCORN_M33_SHARED_DESC": str(
            project_dir / "tests" / "data" / "core" / "m33_shared_page.json"
        ),
        "POPCORN_FW_PERIPH": str(periph_elf),
        **({"POPCORN_U575_DESC": str(u575)} if u575 is not None else {}),
    },
)
if periph_firmware is not None:
    test_shared_page.depends(periph_firmware)
