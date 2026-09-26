from collections.abc import Sequence
from pathlib import Path

from pcons import Target

from popcorn_build.config import project_dir
from popcorn_build.firmware import add_firmware, firmware_env
from popcorn_build.requirements import firmware_source

AREA = "periph"
DEFINES = ("STM32U575xx", "USE_FULL_LL_DRIVER")
SECTION_BOUNDS = (
    "-Wl,--defsym=_sidata=LOADADDR(.data),--defsym=_sdata=ADDR(.data),"
    "--defsym=_edata=ADDR(.data)+SIZEOF(.data),--defsym=_sbss=ADDR(.bss),"
    "--defsym=_ebss=ADDR(.bss)+SIZEOF(.bss)"
)
SUPPORT_DIR = project_dir / "tests" / "firmware" / AREA


def add_ll_firmware(
    name: str,
    sources: Sequence[Path],
    ll_modules: Sequence[str] = (),
    entries: Sequence[str] = (),
) -> tuple[Path, Target | None]:
    """Build tests/firmware/periph/prebuilt/@p name.elf for STM32U575 on ST's LL drivers.

    The firmware links the CMSIS device startup and system files, stm32u5xx_ll_<module>.c for
    each of @p ll_modules, and periph_test.c. @p entries are symbols tests reach that nothing in
    the firmware references, kept through the section garbage collection. Without
    arm-none-eabi-gcc, the ST and ARM packages are not fetched and the committed prebuilt ELF is
    used.
    """
    if firmware_env() is None:
        return add_firmware(AREA, name, sources)
    hal = firmware_source("stm32u5-hal-driver")
    device = firmware_source("cmsis-device-u5")
    cmsis = firmware_source("cmsis-core")
    templates = device / "Source" / "Templates"
    elf, target = add_firmware(
        AREA,
        name,
        [
            *sources,
            SUPPORT_DIR / "periph_test.c",
            templates / "gcc" / "startup_stm32u575xx.s",
            templates / "system_stm32u5xx.c",
            *(hal / "Src" / f"stm32u5xx_ll_{module}.c" for module in ll_modules),
        ],
    )
    assert target is not None
    target.private.include_dirs.extend(
        [SUPPORT_DIR, hal / "Inc", device / "Include", cmsis / "CMSIS" / "Core" / "Include"]
    )
    target.private.defines.extend(DEFINES)
    target.private.link_flags.append(SECTION_BOUNDS)
    target.private.link_libs.append("gcc")
    target.private.link_flags.extend(
        f"-Wl,--undefined={entry}" for entry in ("log_buf", "log_len", *entries)
    )
    return elf, target
