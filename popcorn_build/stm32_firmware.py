from collections.abc import Sequence
from dataclasses import dataclass
from pathlib import Path

from pcons import Target

from popcorn_build.config import project_dir
from popcorn_build.firmware import LINKER_SCRIPT, add_firmware, firmware_env
from popcorn_build.requirements import firmware_source

AREA = "periph"
LL_DEFINE = "USE_FULL_LL_DRIVER"
SECTION_BOUNDS = (
    "-Wl,--defsym=_sidata=LOADADDR(.data),--defsym=_sdata=ADDR(.data),"
    "--defsym=_edata=ADDR(.data)+SIZEOF(.data),--defsym=_sbss=ADDR(.bss),"
    "--defsym=_ebss=ADDR(.bss)+SIZEOF(.bss)"
)
SUPPORT_DIR = project_dir / "tests" / "firmware" / AREA


@dataclass(frozen=True, slots=True)
class Chipset:
    """The ST packages and files that build firmware for one STM32 part.

    @var name Part name, `stm32u575`, used in target names.
    @var define CMSIS device define, `STM32U575xx`.
    @var family Series letter and digit, `u5`, in `stm32<family>xx_ll_<module>.c` and
    `system_stm32<family>xx.c`.
    @var hal_package deps.toml name of the HAL and LL driver package.
    @var device_package deps.toml name of the CMSIS device package.
    @var startup Startup file under the device package's `Source/Templates/gcc`.
    @var system System file under `Source/Templates`.
    @var linker ST's single-world linker script under `Source/Templates/gcc/linker`.
    @var secure_linker ST's Secure linker script, same directory.
    @var nonsecure_linker ST's Non-secure linker script, same directory.
    @var nonsecure_system The Non-secure system file under `Source/Templates`.
    @var test_linker Project-relative linker script of the LL test firmware.
    @var usb_device_sources TinyUSB device controller driver sources under `src`.
    @var prebuilt_dir Subdirectory of `prebuilt` holding the committed ELFs of this chipset,
    empty for the U575.
    """

    name: str
    define: str
    family: str
    hal_package: str
    device_package: str
    startup: str
    system: str
    linker: str
    secure_linker: str
    nonsecure_linker: str
    nonsecure_system: str
    test_linker: str
    usb_device_sources: tuple[str, ...]
    prebuilt_dir: str = ""

    @property
    def defines(self) -> tuple[str, str]:
        return (self.define, LL_DEFINE)


STM32U575 = Chipset(
    name="stm32u575",
    define="STM32U575xx",
    family="u5",
    hal_package="stm32u5-hal-driver",
    device_package="cmsis-device-u5",
    startup="startup_stm32u575xx.s",
    system="system_stm32u5xx.c",
    linker="STM32U575xx_FLASH.ld",
    secure_linker="STM32U575xx_FLASH_s.ld",
    nonsecure_linker="STM32U575xx_FLASH_ns.ld",
    nonsecure_system="system_stm32u5xx_ns.c",
    test_linker=LINKER_SCRIPT,
    usb_device_sources=(
        "portable/synopsys/dwc2/dcd_dwc2.c",
        "portable/synopsys/dwc2/dwc2_common.c",
    ),
)

STM32H573 = Chipset(
    name="stm32h573",
    define="STM32H573xx",
    family="h5",
    hal_package="stm32h5-hal-driver",
    device_package="cmsis-device-h5",
    startup="startup_stm32h573xx.s",
    system="system_stm32h5xx.c",
    linker="STM32H573xx_FLASH.ld",
    secure_linker="STM32H573xx_FLASH_s.ld",
    nonsecure_linker="STM32H573xx_FLASH_ns.ld",
    nonsecure_system="system_stm32h5xx_ns.c",
    test_linker="tests/firmware/common/stm32h573.ld",
    usb_device_sources=(
        "portable/st/stm32_fsdev/dcd_stm32_fsdev.c",
        "portable/st/stm32_fsdev/fsdev_common.c",
    ),
    prebuilt_dir="h573",
)


def add_ll_firmware(
    name: str,
    sources: Sequence[Path],
    ll_modules: Sequence[str] = (),
    entries: Sequence[str] = (),
    chipset: Chipset = STM32U575,
) -> tuple[Path, Target | None]:
    """Build tests/firmware/periph/prebuilt/@p name.elf for @p chipset on ST's LL drivers.

    The ELF of a chipset with a `prebuilt_dir` lives in that subdirectory of `prebuilt`, and is
    declared as `<prebuilt_dir>_<name>`. The firmware links the CMSIS device startup and system
    files, stm32<family>xx_ll_<module>.c for each of @p ll_modules, and periph_test.c. @p entries
    are symbols tests reach that nothing in the firmware references, kept through the section
    garbage collection. Without arm-none-eabi-gcc, the ST and ARM packages are not fetched and the
    committed prebuilt ELF is used.
    """
    if firmware_env() is None:
        return add_firmware(
            AREA,
            name,
            sources,
            subdir=chipset.prebuilt_dir,
            linker_script=chipset.test_linker,
        )
    hal = firmware_source(chipset.hal_package)
    device = firmware_source(chipset.device_package)
    cmsis = firmware_source("cmsis-core")
    templates = device / "Source" / "Templates"
    elf, target = add_firmware(
        AREA,
        name,
        [
            *sources,
            SUPPORT_DIR / "periph_test.c",
            templates / "gcc" / chipset.startup,
            templates / chipset.system,
            *(hal / "Src" / f"stm32{chipset.family}xx_ll_{module}.c" for module in ll_modules),
        ],
        subdir=chipset.prebuilt_dir,
        linker_script=chipset.test_linker,
    )
    assert target is not None
    target.private.include_dirs.extend(
        [SUPPORT_DIR, hal / "Inc", device / "Include", cmsis / "CMSIS" / "Core" / "Include"]
    )
    target.private.defines.extend(chipset.defines)
    target.private.link_flags.append(SECTION_BOUNDS)
    target.private.link_libs.append("gcc")
    target.private.link_flags.extend(
        f"-Wl,--undefined={entry}" for entry in ("log_buf", "log_len", *entries)
    )
    return elf, target
