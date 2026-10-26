import re
from collections.abc import Sequence
from dataclasses import dataclass
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
    trustzone_pair,
)
from popcorn_build.stm32_firmware import STM32U575, Chipset

EXAMPLES_PREFIX = "example-firmware"
HAL_BASE = ("", "cortex", "rcc", "rcc_ex", "gpio", "pwr", "pwr_ex")
FREERTOS_SOURCES = ("tasks.c", "queue.c", "list.c", "timers.c")
FREERTOS_PORT = Path("portable") / "GCC" / "ARM_CM33_NTZ" / "non_secure"
TINYUSB_SOURCES = (
    "tusb.c",
    "common/tusb_fifo.c",
    "device/usbd.c",
)
ROM_REGION = re.compile(
    r"(ROM\s*\(rx\)\s*:\s*ORIGIN\s*=\s*)0x08000000(\s*,\s*LENGTH\s*=\s*)(\d+)K", re.IGNORECASE
)

VECTORS = re.compile(r"([ \t]*)(KEEP\s*\(\s*\*\s*\(\s*\.isr_vector\s*\)\s*\))")
VECTORS_SECTION = re.compile(r"^([ \t]*)\.isr_vector[ \t]*:", re.MULTILINE)
MEMORY_START = re.compile(r"^MEMORY\s*\{[ \t]*\n", re.MULTILINE)
MEMORY_REGION = (
    r"^[ \t]*{name}[ \t]*\((\w+)\)[ \t]*:[ \t]*ORIGIN[ \t]*=[ \t]*0x[0-9A-Fa-f]+[ \t]*,"
    r"[ \t]*LENGTH[ \t]*=[ \t]*\d+K[^\n]*\n"
)
SAU_GRANULE = 32
MPCBB_BLOCK = 512

SECURE_CPU_FLAGS = tuple(
    "-mfloat-abi=softfp" if flag == "-mfloat-abi=hard" else flag for flag in CPU_FLAGS
)


def _newlib_preset(name: str, cpu_flags: Sequence[str]) -> CrossPreset:
    return CrossPreset(
        name=name,
        arch=cortex_m33.arch,
        triple=cortex_m33.triple,
        extra_compile_flags=(*cpu_flags, "-ffunction-sections", "-fdata-sections"),
        extra_link_flags=(
            *cpu_flags,
            "-specs=nano.specs",
            "-specs=nosys.specs",
            "-Wl,--gc-sections",
        ),
        tool_cmds=cortex_m33.tool_cmds,
    )


cortex_m33_newlib = _newlib_preset("cortex-m33-none-eabi-newlib", CPU_FLAGS)
cortex_m33_newlib_softfp = _newlib_preset("cortex-m33-none-eabi-newlib-softfp", SECURE_CPU_FLAGS)

_envs: dict[str, Environment] = {}


def _newlib_env(name: str, preset: CrossPreset) -> Environment | None:
    if name not in _envs and toolchain_found:
        env = context.current_project.Environment(toolchain="gcc", name=name)
        env.apply_cross_preset(preset)
        env.build_prefix = EXAMPLES_PREFIX
        add_c_flags(env)
        _envs[name] = env
    return _envs.get(name)


def example_env() -> Environment | None:
    """The arm-none-eabi environment of the examples, newlib-nano linked, None without the
    cross toolchain."""
    return _newlib_env("examples", cortex_m33_newlib)


def secure_example_env() -> Environment | None:
    """The environment of the examples' secure parts: `example_env` with the softfp ABI, None
    without the cross toolchain."""
    return _newlib_env("examples-secure", cortex_m33_newlib_softfp)


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


@dataclass(frozen=True)
class TrustZoneLayout:
    """Where a secure and Non-secure pair lives, each range a `(start, size)` pair in bytes.

    Attributes:
        secure_flash: The secure part, its vector table first and its NSC veneers in its last
            `nsc_size` bytes. The Non-secure image embeds it at this address.
        nsc_size: Size of the Non-secure callable veneer range.
        secure_ram: Secure data, heap and stack.
        nonsecure_flash: The Non-secure part, its vector table first.
        nonsecure_ram: Non-secure data, heap and stack.

    Raises:
        ValueError: A range is empty, `nsc_size` is not a positive multiple of 32 bytes, the
            SAU granule, below the size of `secure_flash`, the end of `secure_flash` or a bound
            of a Non-secure range is off the SAU granule, or the end of `nonsecure_ram` is off a
            512-byte MPCBB block.
    """

    secure_flash: tuple[int, int]
    nsc_size: int
    secure_ram: tuple[int, int]
    nonsecure_flash: tuple[int, int]
    nonsecure_ram: tuple[int, int]

    def __post_init__(self) -> None:
        for name in ("secure_flash", "secure_ram", "nonsecure_flash", "nonsecure_ram"):
            if getattr(self, name)[1] <= 0:
                raise ValueError(f"the {name} range is empty")
        if not 0 < self.nsc_size < self.secure_flash[1] or self.nsc_size % SAU_GRANULE:
            raise ValueError(
                f"nsc_size is a positive multiple of {SAU_GRANULE} bytes below the secure flash "
                f"size, not {self.nsc_size:#x}"
            )
        bounds = {
            "secure_flash end": sum(self.secure_flash),
            "nonsecure_flash start": self.nonsecure_flash[0],
            "nonsecure_flash end": sum(self.nonsecure_flash),
            "nonsecure_ram start": self.nonsecure_ram[0],
            "nonsecure_ram end": sum(self.nonsecure_ram),
        }
        for name, bound in bounds.items():
            if bound % SAU_GRANULE:
                raise ValueError(
                    f"the {name} is a multiple of the {SAU_GRANULE}-byte SAU granule, "
                    f"not {bound:#x}"
                )
        if sum(self.nonsecure_ram) % MPCBB_BLOCK:
            raise ValueError(
                f"the nonsecure_ram end is a multiple of the {MPCBB_BLOCK}-byte MPCBB block, "
                f"not {sum(self.nonsecure_ram):#x}"
            )


def _region(text: str, name: str, extent: tuple[int, int] | None) -> str:
    pattern = re.compile(MEMORY_REGION.format(name=re.escape(name)), re.MULTILINE)
    if len(pattern.findall(text)) != 1:
        raise ValueError(f"the linker script has no single MEMORY region {name}")
    if extent is None:
        return pattern.sub("", text)
    start, size = extent
    return pattern.sub(
        lambda match: f"  {name}\t({match[1]})\t: ORIGIN = {start:#010x},\tLENGTH = {size:#x}\n",
        text,
    )


def _symbols(**symbols: int) -> str:
    return "".join(f"{name} = {value:#010x};\n" for name, value in symbols.items())


def _nonsecure_symbols(layout: TrustZoneLayout) -> str:
    flash, flash_size = layout.nonsecure_flash
    ram, ram_size = layout.nonsecure_ram
    return _symbols(
        __nonsecure_flash_start=flash,
        __nonsecure_flash_end=flash + flash_size,
        __nonsecure_ram_start=ram,
        __nonsecure_ram_end=ram + ram_size,
    )


def secure_linker_script(text: str, layout: TrustZoneLayout) -> str:
    """ST's secure TrustZone linker script @p text placed by @p layout: ROM, ROM_NSC and RAM
    moved, SRAM4 dropped, the veneer range bounds as `__sgstubs_start` and `__sgstubs_end`, and
    the Non-secure ranges as `__nonsecure_flash_start`, `_end`, `__nonsecure_ram_start` and
    `_end`, for the SAU and the MPCBB setup.

    @throws ValueError @p text has no single RAM, ROM, ROM_NSC or SRAM4 region.
    """
    start, size = layout.secure_flash
    nsc = start + size - layout.nsc_size
    text = _region(text, "ROM", (start, size - layout.nsc_size))
    text = _region(text, "ROM_NSC", (nsc, layout.nsc_size))
    text = _region(text, "RAM", layout.secure_ram)
    text = _region(text, "SRAM4", None)
    return (
        text
        + "\n__sgstubs_start = ORIGIN(ROM_NSC);\n"
        + "__sgstubs_end = ORIGIN(ROM_NSC) + LENGTH(ROM_NSC);\n"
        + _nonsecure_symbols(layout)
        + '\nASSERT(ADDR(.gnu.sgstubs) == ORIGIN(ROM_NSC), "the veneers start the NSC range")\n'
    )


def nonsecure_linker_script(text: str, layout: TrustZoneLayout) -> str:
    """ST's Non-secure TrustZone linker script @p text placed by @p layout: ROM and RAM moved,
    SRAM4 dropped, and a SECURE region over the secure part holding the `.secure_image` input
    section, placed first and checked against that region. `__nonsecure_ram_end` is the end of
    RAM.

    @throws ValueError @p text has no single RAM, ROM or SRAM4 region, no MEMORY block or no
    single `.isr_vector` output section.
    """
    if len(VECTORS_SECTION.findall(text)) != 1:
        raise ValueError("the linker script has no single .isr_vector output section")
    if len(MEMORY_START.findall(text)) != 1:
        raise ValueError("the linker script has no single MEMORY block")
    text = _region(text, "ROM", layout.nonsecure_flash)
    text = _region(text, "RAM", layout.nonsecure_ram)
    text = _region(text, "SRAM4", None)
    start, size = layout.secure_flash
    text = MEMORY_START.sub(
        lambda match: f"{match[0]}  SECURE\t(rx)\t: ORIGIN = {start:#010x},\tLENGTH = {size:#x}\n",
        text,
    )
    text = VECTORS_SECTION.sub(
        lambda match: (
            f"{match[1]}.secure_image :\n{match[1]}{{\n{match[1]}  KEEP(*(.secure_image))\n"
            f"{match[1]}}} >SECURE\n\n{match[0]}"
        ),
        text,
    )
    return (
        text
        + "\n"
        + _nonsecure_symbols(layout)
        + '\nASSERT(ADDR(.secure_image) == ORIGIN(SECURE), "the secure part starts its range")\n'
        + 'ASSERT(SIZEOF(.secure_image) > 0, "the secure part is embedded")\n'
        + 'ASSERT(SIZEOF(.secure_image) <= LENGTH(SECURE), "the secure part fits its range")\n'
    )


def _derived_script(chipset: Chipset, name: str, text: str) -> Path:
    derived = build_dir / EXAMPLES_PREFIX / chipset.name / f"{name}.ld"
    if not derived.is_file() or derived.read_text() != text:
        derived.parent.mkdir(parents=True, exist_ok=True)
        derived.write_text(text)
    return derived


def _templates(device: Path) -> Path:
    return device / "Source" / "Templates"


def _linker_script(chipset: Chipset, device: Path, name: str, flash_offset: int) -> Path:
    script = _templates(device) / "gcc" / "linker" / chipset.linker
    if flash_offset == 0:
        return script
    return _derived_script(chipset, name, offset_linker_script(script.read_text(), flash_offset))


def add_example_firmware(
    example_dir: Path,
    name: str,
    sources: Sequence[Path],
    *,
    hal: Sequence[str] = (),
    ll: Sequence[str] = (),
    freertos: bool = False,
    tinyusb: Sequence[str] = (),
    defines: Sequence[str] = (),
    flash_offset: int = 0,
    chipset: Chipset = STM32U575,
) -> tuple[Path, Target | None]:
    """Build @p example_dir/prebuilt/@p name.elf as a user's @p chipset Cube project would, or
    fall back to that committed copy without arm-none-eabi-gcc.

    The firmware links ST's startup, system file and FLASH linker script with newlib-nano, the
    HAL modules @p hal with the ones every HAL project needs, the LL modules @p ll, and with
    @p freertos the FreeRTOS kernel on its non-secure Cortex-M33 port and heap_4, and with
    @p tinyusb the TinyUSB device stack, the device driver of @p chipset and the device class
    drivers named, such as `cdc` and `hid`. Headers are looked up in @p example_dir/firmware
    first, where a TinyUSB firmware keeps its `tusb_config.h`.

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

    driver = firmware_source(chipset.hal_package)
    device = firmware_source(chipset.device_package)
    cmsis = firmware_source("cmsis-core")
    templates = device / "Source" / "Templates"
    family = f"stm32{chipset.family}xx"
    hal_modules = [*HAL_BASE, *(m for m in hal if m not in HAL_BASE)] if hal else []
    all_sources = [
        *sources,
        templates / "gcc" / chipset.startup,
        templates / chipset.system,
        *(driver / "Src" / f"{family}_hal{'_' if m else ''}{m}.c" for m in hal_modules),
        *(driver / "Src" / f"{family}_ll_{m}.c" for m in ll),
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
    if tinyusb:
        stack = firmware_source("tinyusb") / "src"
        device_sources = (*TINYUSB_SOURCES, *chipset.usb_device_sources)
        all_sources += [stack / source for source in device_sources]
        all_sources += [stack / "class" / name / f"{name}_device.c" for name in tinyusb]
        include_dirs.append(stack)

    script = _linker_script(chipset, device, name, flash_offset)
    elf, target = checked_program(
        f"example-{chipset.name}-{name}",
        name,
        env,
        all_sources,
        PathToken(prefix="-T", path=script, path_type="absolute"),
        script,
        prebuilt,
    )
    target.private.include_dirs.extend([example_dir / "firmware", *include_dirs])
    target.private.defines.extend(
        [
            chipset.define,
            *(["USE_HAL_DRIVER"] if hal else []),
            *(["USE_FULL_LL_DRIVER"] if ll else []),
            *defines,
        ]
    )
    return elf, target


def add_trustzone_example_firmware(
    example_dir: Path,
    name: str,
    secure_sources: Sequence[Path],
    nonsecure_sources: Sequence[Path],
    layout: TrustZoneLayout,
) -> tuple[Path, Path, Target | None]:
    """Build @p example_dir/prebuilt/@p name_s.elf and @p name.elf, a secure and Non-secure
    STM32U575 pair as `popcorn_build.firmware.trustzone_pair` links it, or fall back to those
    committed copies without arm-none-eabi-gcc.

    Both parts link ST's startup with newlib-nano and the CMSIS device and core headers, from
    linker scripts derived from ST's `_s` and `_ns` templates by `secure_linker_script` and
    `nonsecure_linker_script` with @p layout. The Non-secure part also links ST's Non-secure
    system file. The secure part brings its own `SystemInit`, and is built with
    `-mfloat-abi=softfp` so its calls to Non-secure code save the floating-point context with
    VLSTM and VLLDM. The Non-secure part keeps the hard-float ABI: the two meet only through
    integer arguments. Headers are looked up in @p example_dir/firmware first.

    @return The secure and the Non-secure ELF paths, and the Non-secure target, None when the
    prebuilt copies are used.
    """
    prebuilt = example_dir / "prebuilt"
    env = example_env()
    secure_env = secure_example_env()
    if env is None or secure_env is None:
        return (
            prebuilt_only(prebuilt / f"{name}_s.elf"),
            prebuilt_only(prebuilt / f"{name}.elf"),
            None,
        )

    from popcorn_build.requirements import firmware_source

    chipset = STM32U575
    device = firmware_source(chipset.device_package)
    cmsis = firmware_source("cmsis-core")
    templates = _templates(device)
    linker = templates / "gcc" / "linker"
    startup = templates / "gcc" / chipset.startup
    secure_ld = _derived_script(
        chipset,
        f"{name}_s",
        secure_linker_script((linker / chipset.secure_linker).read_text(), layout),
    )
    nonsecure_ld = _derived_script(
        chipset,
        name,
        nonsecure_linker_script((linker / chipset.nonsecure_linker).read_text(), layout),
    )
    secure_path, secure, nonsecure_path, nonsecure = trustzone_pair(
        f"example-{chipset.name}-{name}",
        name,
        secure_env,
        env,
        [*secure_sources, startup],
        [*nonsecure_sources, startup, templates / chipset.nonsecure_system],
        secure_ld,
        nonsecure_ld,
        prebuilt,
    )
    for target in (secure, nonsecure):
        target.private.include_dirs.extend(
            [example_dir / "firmware", device / "Include", cmsis / "CMSIS" / "Core" / "Include"]
        )
        target.private.defines.append(chipset.define)
    return secure_path, nonsecure_path, nonsecure
