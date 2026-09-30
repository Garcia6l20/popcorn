import re
from collections.abc import Sequence
from pathlib import Path

from pcons import PathToken, Target, context
from pcons.core.environment import Environment
from pcons.toolchains.presets import CrossPreset

from popcorn_build.config import build_dir
from popcorn_build.firmware import (
    CPU_FLAGS,
    add_c_flags,
    checked_program,
    cortex_m33,
    prebuilt_only,
    toolchain_found,
)

CHIPSET = "stm32u575"
EXAMPLES_PREFIX = "example-firmware"
HAL_BASE = ("", "cortex", "rcc", "rcc_ex", "gpio", "pwr", "pwr_ex")
FREERTOS_SOURCES = ("tasks.c", "queue.c", "list.c", "timers.c")
FREERTOS_PORT = Path("portable") / "GCC" / "ARM_CM33_NTZ" / "non_secure"
ROM_REGION = re.compile(
    r"(ROM\s*\(rx\)\s*:\s*ORIGIN\s*=\s*)0x08000000(\s*,\s*LENGTH\s*=\s*)(\d+)K", re.IGNORECASE
)

VECTORS = re.compile(r"([ \t]*)(KEEP\s*\(\s*\*\s*\(\s*\.isr_vector\s*\)\s*\))")

cortex_m33_newlib = CrossPreset(
    name="cortex-m33-none-eabi-newlib",
    arch=cortex_m33.arch,
    triple=cortex_m33.triple,
    extra_compile_flags=(*CPU_FLAGS, "-ffunction-sections", "-fdata-sections"),
    extra_link_flags=(*CPU_FLAGS, "-specs=nano.specs", "-specs=nosys.specs", "-Wl,--gc-sections"),
    tool_cmds=cortex_m33.tool_cmds,
)

_env: Environment | None = None


def example_env() -> Environment | None:
    """The arm-none-eabi environment of the examples, newlib-nano linked, None without the
    cross toolchain."""
    global _env
    if _env is None and toolchain_found:
        _env = context.current_project.Environment(toolchain="gcc", name="examples")
        _env.apply_cross_preset(cortex_m33_newlib)
        _env.build_prefix = EXAMPLES_PREFIX
        add_c_flags(_env)
    return _env


def prebuilt_elf(example_dir: Path, name: str) -> Path:
    """The committed ELF of firmware @p name of the example in @p example_dir."""
    return example_dir / "prebuilt" / f"{name}.elf"


def offset_linker_script(text: str, offset: int) -> str:
    """ST's linker script @p text with its ROM region starting @p offset bytes into flash, and
    the input section `.app_header` placed first, ahead of the vector table.

    @throws ValueError @p offset is not a positive multiple of 1 KiB, or the script has no single
    ROM region at 0x08000000 or no single `KEEP(*(.isr_vector))`.
    """
    if offset <= 0 or offset % 1024:
        raise ValueError(f"a flash offset is a positive multiple of 1 KiB, not {offset:#x}")
    if len(ROM_REGION.findall(text)) != 1:
        raise ValueError("the linker script has no single ROM region at 0x08000000")
    if len(VECTORS.findall(text)) != 1:
        raise ValueError("the linker script has no single KEEP(*(.isr_vector))")
    text = VECTORS.sub(lambda match: f"{match[1]}KEEP(*(.app_header))\n{match[1]}{match[2]}", text)
    return ROM_REGION.sub(
        lambda match: (
            f"{match[1]}{0x0800_0000 + offset:#010x}{match[2]}{int(match[3]) - offset // 1024}K"
        ),
        text,
    )


def _linker_script(device: Path, name: str, flash_offset: int) -> Path:
    script = device / "Source" / "Templates" / "gcc" / "linker" / "STM32U575xx_FLASH.ld"
    if flash_offset == 0:
        return script
    derived = build_dir / EXAMPLES_PREFIX / CHIPSET / f"{name}.ld"
    text = offset_linker_script(script.read_text(), flash_offset)
    if not derived.is_file() or derived.read_text() != text:
        derived.parent.mkdir(parents=True, exist_ok=True)
        derived.write_text(text)
    return derived


def add_example_firmware(
    example_dir: Path,
    name: str,
    sources: Sequence[Path],
    *,
    hal: Sequence[str] = (),
    ll: Sequence[str] = (),
    freertos: bool = False,
    defines: Sequence[str] = (),
    flash_offset: int = 0,
) -> tuple[Path, Target | None]:
    """Build @p example_dir/prebuilt/@p name.elf as a user's STM32U575 Cube project would, or
    fall back to that committed copy without arm-none-eabi-gcc.

    The firmware links ST's startup, system file and STM32U575xx_FLASH.ld with newlib-nano, the
    HAL modules @p hal with the ones every HAL project needs, the LL modules @p ll, and with
    @p freertos the FreeRTOS kernel on its non-secure Cortex-M33 port and heap_4. Headers are
    looked up in @p example_dir/firmware first.

    @param flash_offset Link the image this many bytes into flash, as an application behind a
    bootloader is. 0 keeps ST's layout.
    @return The ELF path and the target building it, None when the prebuilt copy is used. A
    firmware-check-@p name target fails when the rebuilt ELF differs from the prebuilt one in
    loadable content.
    """
    prebuilt = prebuilt_elf(example_dir, name)
    env = example_env()
    if env is None:
        return prebuilt_only(prebuilt), None

    from popcorn_build.requirements import firmware_source

    driver = firmware_source("stm32u5-hal-driver")
    device = firmware_source("cmsis-device-u5")
    cmsis = firmware_source("cmsis-core")
    templates = device / "Source" / "Templates"
    hal_modules = [*HAL_BASE, *(m for m in hal if m not in HAL_BASE)] if hal else []
    all_sources = [
        *sources,
        templates / "gcc" / "startup_stm32u575xx.s",
        templates / "system_stm32u5xx.c",
        *(driver / "Src" / f"stm32u5xx_hal{'_' if m else ''}{m}.c" for m in hal_modules),
        *(driver / "Src" / f"stm32u5xx_ll_{m}.c" for m in ll),
    ]
    include_dirs = [
        driver / "Inc",
        device / "Include",
        cmsis / "CMSIS" / "Core" / "Include",
    ]
    if freertos:
        kernel = firmware_source("freertos-kernel")
        port = kernel / FREERTOS_PORT
        all_sources += [kernel / source for source in FREERTOS_SOURCES]
        all_sources += [port / "port.c", port / "portasm.c", kernel / "portable/MemMang/heap_4.c"]
        include_dirs += [kernel / "include", port]

    script = _linker_script(device, name, flash_offset)
    elf, target = checked_program(
        f"example-{CHIPSET}-{name}",
        name,
        env,
        all_sources,
        PathToken(prefix="-T", path=script, path_type="absolute"),
        script,
        prebuilt,
    )
    target.private.compile_flags.append(
        PathToken(prefix="-I", path=example_dir / "firmware", path_type="absolute")
    )
    target.private.include_dirs.extend(include_dirs)
    target.private.defines.extend(
        [
            "STM32U575xx",
            *(["USE_HAL_DRIVER"] if hal else []),
            *(["USE_FULL_LL_DRIVER"] if ll else []),
            *defines,
        ]
    )
    return elf, target
