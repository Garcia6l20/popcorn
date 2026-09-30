import shutil
import sys
from collections.abc import Sequence
from pathlib import Path

from pcons import PathToken, Target, context
from pcons.core.environment import Environment
from pcons.toolchains.presets import CrossPreset

from popcorn_build.config import project_dir

CPU_FLAGS = ("-mcpu=cortex-m33", "-mthumb", "-mfloat-abi=hard", "-mfpu=fpv5-sp-d16")
LINKER_SCRIPT = "tests/firmware/common/m33.ld"
FIRMWARE_PREFIX = "firmware"

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
_declared: dict[tuple[str, str], tuple[Path, Target | None]] = {}


def firmware_env() -> Environment | None:
    """The arm-none-eabi environment, or None when the cross toolchain is not on PATH."""
    global _env
    if _env is None and toolchain_found:
        project = context.current_project
        _env = project.Environment(toolchain="gcc", name="firmware")
        _env.apply_cross_preset(cortex_m33)
        _env.build_prefix = FIRMWARE_PREFIX
        add_c_flags(_env)
    return _env


def add_c_flags(env: Environment) -> None:
    """C23, size-optimized with debug info, warnings as errors, project paths made relative."""
    env.cc.flags.extend(["-std=c23", "-Os", "-g", "-Wall", "-Wextra", "-Werror"])
    env.cc.flags.append(
        PathToken(prefix="-ffile-prefix-map=", path=project_dir, path_type="absolute", suffix="=.")
    )


def prebuilt_only(prebuilt: Path) -> Path:
    """@p prebuilt, the committed ELF used without the cross toolchain.

    @throws SystemExit The prebuilt ELF does not exist.
    """
    if not prebuilt.is_file():
        raise SystemExit(f"popcorn: no arm-none-eabi-gcc and no prebuilt firmware {prebuilt}")
    return prebuilt


def checked_program(
    target_name: str,
    name: str,
    env: Environment,
    sources: Sequence[str | Path],
    linker_script: PathToken,
    linker_file: Path,
    prebuilt: Path,
) -> tuple[Path, Target]:
    """Link @p name.elf from @p sources with @p linker_script, checked against @p prebuilt.

    @return The ELF path and its target. A firmware-check-@p name target fails when the loadable
    content of the ELF differs from @p prebuilt.
    """
    project = context.current_project
    elf = project.Program(target_name, env, sources=list(sources))
    elf.output_name = name
    elf.output_suffix = ".elf"
    elf.private.link_flags.append(linker_script)
    elf.depends(linker_file)
    elf_dir = elf.build_dir if elf.build_dir.is_absolute() else project.top.root_dir / elf.build_dir
    elf_path = elf_dir / f"{name}.elf"

    stamp = elf_dir / f"{name}.check"
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


def add_firmware(area: str, name: str, sources: Sequence[str | Path]) -> tuple[Path, Target | None]:
    """Build tests/firmware/@p area/@p name.elf, or fall back to its committed prebuilt copy.

    @return The ELF path tests load and the target building it, None when the prebuilt copy is used.
    With the toolchain, a firmware-check-@p name target fails when the loadable content of the
    rebuilt ELF differs from the prebuilt one.
    """
    prebuilt = project_dir / "tests" / "firmware" / area / "prebuilt" / f"{name}.elf"
    env = firmware_env()
    if env is None:
        _declared[area, name] = (prebuilt_only(prebuilt), None)
        return prebuilt, None

    elf_path, elf = checked_program(
        f"firmware-{area}-{name}",
        name,
        env,
        sources,
        PathToken(prefix="-T", path=LINKER_SCRIPT, path_type="project"),
        project_dir / LINKER_SCRIPT,
        prebuilt,
    )
    _declared[area, name] = (elf_path, elf)
    return elf_path, elf


def firmware(area: str, name: str) -> tuple[Path, Target | None]:
    """The ELF path and target add_firmware returned for @p area/@p name.

    Build scripts under tests/firmware run before those under tests/native, so a native test
    reaches firmware declared in another directory through this lookup.
    """
    declared = _declared.get((area, name))
    if declared is None:
        raise SystemExit(f"popcorn: firmware {area}/{name} is not declared, see tests/firmware")
    return declared
