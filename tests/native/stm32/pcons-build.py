from pathlib import Path

from pcons import context

from popcorn_build.config import project_dir
from popcorn_build.firmware import firmware
from popcorn_build.st_description import cached_st_description
from popcorn_build.testing import add_doctest

here = Path(__file__).resolve().parent
project = context.current_project
core = project.get_target("popcorn_core")
private_dir = project_dir / "native" / "src" / "periph" / "stm32"
periph_data = project_dir / "tests" / "data" / "periph"

u575 = cached_st_description("STM32U575QGIx", "POPCORN_U575_DESC")
firmwares = {name: firmware("periph", name) for name in ("boot_ll", "rcc_ll")}
u575_env = {
    "POPCORN_U575_DESC": str(u575),
    **{f"POPCORN_FW_{name.upper()}": str(elf) for name, (elf, _) in firmwares.items()},
}

base = add_doctest(
    "stm32-base",
    [here / "test_base.cpp"],
    [core],
    env={"POPCORN_STM32_BASE_DESC": str(periph_data / "stm32_base.json")},
)
base.private.include_dirs.append(private_dir)

u575_programs = [
    add_doctest(name, [here / source], [core], env=u575_env, register=u575 is not None)
    for name, source in (
        ("stm32-attach-u575", "test_attach_u575.cpp"),
        ("stm32-rcc", "test_rcc.cpp"),
        ("stm32-caches", "test_caches.cpp"),
    )
]
for program in u575_programs:
    program.private.include_dirs.append(private_dir)
    for _, target in firmwares.values():
        if target is not None:
            program.depends(target)
