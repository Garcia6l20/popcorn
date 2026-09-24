import shutil
import sys
from collections.abc import Sequence
from pathlib import Path

from pcons import PathToken, Target, context
from pcons.core.environment import Environment
from pcons.toolchains.presets import CrossPreset

from popcorn_build.config import build_dir, project_dir

CPU_FLAGS = ("-mcpu=cortex-m33", "-mthumb", "-mfloat-abi=hard", "-mfpu=fpv5-sp-d16")
LINKER_SCRIPT = "tests/firmware/common/m33.ld"

cortex_m33 = CrossPreset(
    name="cortex-m33-none-eabi",
    arch="armv8-m.main",
    triple="arm-none-eabi",
    extra_compile_flags=(*CPU_FLAGS, "-ffreestanding", "-ffunction-sections", "-fdata-sections"),
    extra_link_flags=(*CPU_FLAGS, "-nostdlib", "-Wl,--gc-sections"),
    tool_cmds={
        "cc": "arm-none-eabi-gcc",
        "cxx": "arm-none-eabi-g++",
        "link": "arm-none-eabi-gcc",
        "ar": "arm-none-eabi-ar",
    },
)

toolchain_found = all(
    shutil.which(tool) is not None for tool in ("arm-none-eabi-gcc", "arm-none-eabi-objcopy")
)

_env: Environment | None = None


def firmware_env() -> Environment | None:
    """The arm-none-eabi environment, or None when the cross toolchain is not on PATH."""
    global _env
    if _env is None and toolchain_found:
        project = context.current_project
        _env = project.Environment(toolchain="gcc", name="firmware")
        _env.apply_cross_preset(cortex_m33)
        _env.build_prefix = "firmware"
        _env.cc.flags.extend(["-std=c23", "-Os", "-g", "-Wall", "-Wextra", "-Werror"])
        _env.cc.flags.append(
            PathToken(
                prefix="-ffile-prefix-map=", path=project_dir, path_type="absolute", suffix="=."
            )
        )
    return _env


def add_firmware(area: str, name: str, sources: Sequence[str]) -> tuple[Path, Target | None]:
    """Build tests/firmware/@p area/@p name.elf, or fall back to its committed prebuilt copy.

    @return The ELF path tests load and the target building it, None when the prebuilt copy is used.
    With the toolchain, a firmware-check-@p name target fails when the loadable content of the
    rebuilt ELF differs from the prebuilt one.
    """
    prebuilt = project_dir / "tests" / "firmware" / area / "prebuilt" / f"{name}.elf"
    env = firmware_env()
    if env is None:
        if not prebuilt.is_file():
            raise SystemExit(f"popcorn: no arm-none-eabi-gcc and no prebuilt firmware {prebuilt}")
        return prebuilt, None

    project = context.current_project
    elf = project.Program(f"firmware-{area}-{name}", env, sources=list(sources))
    elf.output_name = f"{area}/{name}"
    elf.output_suffix = ".elf"
    elf.private.link_flags.append(PathToken(prefix="-T", path=LINKER_SCRIPT, path_type="project"))
    elf.depends(LINKER_SCRIPT)
    elf_path = build_dir / "firmware" / area / f"{name}.elf"

    stamp = build_dir / "firmware" / area / f"{name}.check"
    check = env.Command(
        target=stamp,
        source=[elf, prebuilt],
        command=[
            sys.executable,
            str(Path(__file__).with_name("firmware_check.py")),
            "arm-none-eabi-objcopy",
            str(elf_path),
            str(prebuilt),
            str(stamp),
        ],
    )
    project.Alias("firmware-check", check)
    return elf_path, elf
