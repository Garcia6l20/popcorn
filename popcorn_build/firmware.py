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


def add_trustzone_firmware(
    area: str,
    name: str,
    secure_sources: Sequence[str | Path],
    nonsecure_sources: Sequence[str | Path],
    secure_ld: Path,
    nonsecure_ld: Path,
) -> tuple[Path, Path, Target | None]:
    """Build a secure and Non-secure pair: tests/firmware/@p area/@p name_s.elf, linked with
    -mcmse and its CMSE import library, and @p name.elf, linked against that library with the
    secure ELF's `objcopy -O binary` image in its `.secure_image` section. Without the cross
    toolchain both come from their committed prebuilt copies.

    @return The secure and the Non-secure ELF paths, and the Non-secure target, None when the
    prebuilt copies are used. Each ELF has a firmware-check target against its prebuilt copy.
    """
    prebuilt = project_dir / "tests" / "firmware" / area / "prebuilt"
    secure_prebuilt = prebuilt / f"{name}_s.elf"
    nonsecure_prebuilt = prebuilt / f"{name}.elf"
    env = firmware_env()
    if env is None:
        _declared[area, f"{name}_s"] = (prebuilt_only(secure_prebuilt), None)
        _declared[area, name] = (prebuilt_only(nonsecure_prebuilt), None)
        return secure_prebuilt, nonsecure_prebuilt, None

    project = context.current_project
    secure_path, secure = checked_program(
        f"firmware-{area}-{name}_s",
        f"{name}_s",
        env,
        secure_sources,
        PathToken(prefix="-T", path=secure_ld, path_type="absolute"),
        secure_ld,
        secure_prebuilt,
    )
    implib = secure_path.with_name(f"{name}_s_implib.o")
    secure.private.compile_flags.append("-mcmse")
    secure.private.link_flags.append(f"-Wl,--cmse-implib,--out-implib={implib}")
    secure.private.link_libs.append("gcc")
    image = secure_path.with_name(f"{name}_s_image.o")
    embedded = env.Command(
        target=image,
        source=[secure],
        command=[
            sys.executable,
            str(Path(__file__).with_name("secure_image.py")),
            "arm-none-eabi-objcopy",
            str(secure_path),
            str(image),
            *CPU_FLAGS,
        ],
    )
    nonsecure_path, nonsecure = checked_program(
        f"firmware-{area}-{name}",
        name,
        env,
        nonsecure_sources,
        PathToken(prefix="-T", path=nonsecure_ld, path_type="absolute"),
        nonsecure_ld,
        nonsecure_prebuilt,
    )
    nonsecure.private.link_flags.extend([str(implib), str(image)])
    nonsecure.depends(secure)
    nonsecure.depends(embedded)
    _declared[area, f"{name}_s"] = (secure_path, secure)
    _declared[area, name] = (nonsecure_path, nonsecure)
    project.Alias(f"firmware-{area}-{name}-pair", secure, nonsecure)
    return secure_path, nonsecure_path, nonsecure


def firmware(area: str, name: str) -> tuple[Path, Target | None]:
    """The ELF path and target add_firmware returned for @p area/@p name.

    Build scripts under tests/firmware run before those under tests/native, so a native test
    reaches firmware declared in another directory through this lookup.
    """
    declared = _declared.get((area, name))
    if declared is None:
        raise SystemExit(f"popcorn: firmware {area}/{name} is not declared, see tests/firmware")
    return declared
