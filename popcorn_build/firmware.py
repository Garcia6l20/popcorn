import shutil
import sys
from collections.abc import Sequence
from pathlib import Path

from pcons import PathToken, Target, context
from pcons.core.environment import Environment
from pcons.toolchains.presets import CrossPreset

from popcorn_build.config import project_dir

CPU_FLAGS = ("-mcpu=cortex-m33", "-mthumb", "-mfloat-abi=hard", "-mfpu=fpv5-sp-d16")
CPU_FLAGS_M3 = ("-mcpu=cortex-m3", "-mthumb", "-mfloat-abi=soft")
CPU_FLAGS_M4F = ("-mcpu=cortex-m4", "-mthumb", "-mfpu=fpv4-sp-d16", "-mfloat-abi=hard")
LINKER_SCRIPT = "tests/firmware/common/m33.ld"
FIRMWARE_PREFIX = "firmware"


def _preset(name: str, arch: str, cpu_flags: Sequence[str]) -> CrossPreset:
    return CrossPreset(
        name=name,
        arch=arch,
        triple="arm-none-eabi",
        extra_compile_flags=(
            *cpu_flags,
            "-ffreestanding",
            "-ffunction-sections",
            "-fdata-sections",
        ),
        extra_link_flags=(*cpu_flags, "-nostdlib", "-Wl,--gc-sections"),
        tool_cmds={
            "cc": "arm-none-eabi-gcc",
            "cxx": "arm-none-eabi-g++",
            "link": "arm-none-eabi-gcc",
            "ar": "arm-none-eabi-ar",
        },
    )


cortex_m33 = _preset("cortex-m33-none-eabi", "armv8-m.main", CPU_FLAGS)
cortex_m3 = _preset("cortex-m3-none-eabi", "armv7-m", CPU_FLAGS_M3)
cortex_m4f = _preset("cortex-m4f-none-eabi", "armv7e-m", CPU_FLAGS_M4F)

FIRMWARE_PRESETS = {
    cortex_m33.name: (cortex_m33, FIRMWARE_PREFIX),
    cortex_m3.name: (cortex_m3, f"{FIRMWARE_PREFIX}-m3"),
    cortex_m4f.name: (cortex_m4f, f"{FIRMWARE_PREFIX}-m4f"),
}

toolchain_found = all(
    shutil.which(tool) is not None for tool in ("arm-none-eabi-gcc", "arm-none-eabi-objcopy")
)

_envs: dict[str, Environment] = {}
_declared: dict[tuple[str, str], tuple[Path, Target | None]] = {}


def firmware_env(preset: CrossPreset = cortex_m33) -> Environment | None:
    """The arm-none-eabi environment of @p preset, or None when the cross toolchain is not on PATH.

    Each preset has its own environment and its own build prefix, `firmware` for the Cortex-M33,
    `firmware-m3` and `firmware-m4f` for the others, so a source built for two CPUs gets one
    object path per CPU.

    @throws SystemExit @p preset is not equal to one of cortex_m33, cortex_m3 and cortex_m4f.
    """
    registered = FIRMWARE_PRESETS.get(preset.name)
    if registered is None:
        raise SystemExit(f"popcorn: no firmware build prefix for the preset {preset.name}")
    if registered[0] != preset:
        raise SystemExit(f"popcorn: the registered preset {preset.name}, flags differ")
    prefix = registered[1]
    if preset.name not in _envs and toolchain_found:
        env = context.current_project.Environment(toolchain="gcc", name=prefix)
        env.apply_cross_preset(preset)
        env.build_prefix = prefix
        add_c_flags(env)
        _envs[preset.name] = env
    return _envs.get(preset.name)


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


def add_firmware(
    area: str,
    name: str,
    sources: Sequence[str | Path],
    subdir: str = "",
    linker_script: str = LINKER_SCRIPT,
    preset: CrossPreset = cortex_m33,
) -> tuple[Path, Target | None]:
    """Build tests/firmware/@p area/@p name.elf, or fall back to its committed prebuilt copy.

    With a @p subdir the prebuilt copy is prebuilt/@p subdir/@p name.elf, the target is
    firmware-@p area-@p subdir_@p name and `firmware` finds it as @p subdir_@p name. The firmware
    links with @p linker_script, a project-relative path, in the environment of @p preset.

    @return The ELF path tests load and the target building it, None when the prebuilt copy is used.
    With the toolchain, a firmware-check-@p name target fails when the loadable content of the
    rebuilt ELF differs from the prebuilt one.
    """
    prebuilt = project_dir / "tests" / "firmware" / area / "prebuilt" / subdir / f"{name}.elf"
    declared = f"{subdir}_{name}" if subdir else name
    env = firmware_env(preset)
    if env is None:
        _declared[area, declared] = (prebuilt_only(prebuilt), None)
        return prebuilt, None

    elf_path, elf = checked_program(
        f"firmware-{area}-{declared}",
        declared,
        env,
        sources,
        PathToken(prefix="-T", path=linker_script, path_type="project"),
        project_dir / linker_script,
        prebuilt,
    )
    _declared[area, declared] = (elf_path, elf)
    return elf_path, elf


def trustzone_pair(
    target_prefix: str,
    name: str,
    secure_env: Environment,
    nonsecure_env: Environment,
    secure_sources: Sequence[str | Path],
    nonsecure_sources: Sequence[str | Path],
    secure_ld: Path,
    nonsecure_ld: Path,
    prebuilt: Path,
) -> tuple[Path, Target, Path, Target]:
    """Link a secure and Non-secure pair: @p name_s.elf with @p secure_env, compiled with -mcmse
    and linked with its CMSE import library, and @p name.elf with @p nonsecure_env, linked
    against that library with the secure ELF's `objcopy -O binary` image in its `.secure_image`
    section.

    @return The secure ELF path and target, then the Non-secure ones. Each ELF has a
    firmware-check target against its copy in @p prebuilt, and a third one fails when the
    Non-secure ELF's `.secure_image` differs from the secure ELF's binary.
    """
    project = context.current_project
    secure_path, secure = checked_program(
        f"{target_prefix}_s",
        f"{name}_s",
        secure_env,
        secure_sources,
        PathToken(prefix="-T", path=secure_ld, path_type="absolute"),
        secure_ld,
        prebuilt / f"{name}_s.elf",
    )
    implib = secure_path.with_name(f"{name}_s_implib.o")
    secure.private.compile_flags.append("-mcmse")
    secure.private.link_flags.append(f"-Wl,--cmse-implib,--out-implib={implib}")
    secure.private.link_libs.append("gcc")
    image = secure_path.with_name(f"{name}_s_image.o")
    script = Path(__file__).with_name("secure_image.py")
    embedded = nonsecure_env.Command(
        target=image,
        source=[secure, script],
        command=[
            sys.executable,
            str(script),
            "arm-none-eabi-objcopy",
            str(secure_path),
            str(image),
            *CPU_FLAGS,
        ],
    )
    nonsecure_path, nonsecure = checked_program(
        target_prefix,
        name,
        nonsecure_env,
        nonsecure_sources,
        PathToken(prefix="-T", path=nonsecure_ld, path_type="absolute"),
        nonsecure_ld,
        prebuilt / f"{name}.elf",
    )
    nonsecure.private.link_flags.extend([str(implib), str(image)])
    nonsecure.depends(secure)
    nonsecure.depends(embedded)
    stamp = nonsecure_path.with_name(f"{name}_image.check")
    check = nonsecure_env.Command(
        target=stamp,
        source=[secure, nonsecure, script],
        command=[
            sys.executable,
            str(script),
            "--check",
            "arm-none-eabi-objcopy",
            str(secure_path),
            str(nonsecure_path),
            str(stamp),
        ],
    )
    project.Alias("firmware-check", check)
    project.Alias(f"{target_prefix}-pair", secure, nonsecure)
    return secure_path, secure, nonsecure_path, nonsecure


def add_trustzone_firmware(
    area: str,
    name: str,
    secure_sources: Sequence[str | Path],
    nonsecure_sources: Sequence[str | Path],
    secure_ld: Path,
    nonsecure_ld: Path,
    subdir: str = "",
    include_dirs: Sequence[Path] = (),
    defines: Sequence[str] = (),
) -> tuple[Path, Path, Target | None]:
    """Build the trustzone_pair tests/firmware/@p area/@p name_s.elf and @p name.elf with the
    test firmware flags. Without the cross toolchain both come from their committed prebuilt
    copies.

    With a @p subdir the prebuilt copies are prebuilt/@p subdir/@p name_s.elf and @p name.elf,
    declared as @p subdir_@p name_s and @p subdir_@p name. Both parts compile with
    @p include_dirs and @p defines.

    @return The secure and the Non-secure ELF paths, and the Non-secure target, None when the
    prebuilt copies are used.
    """
    prebuilt = project_dir / "tests" / "firmware" / area / "prebuilt" / subdir
    declared = f"{subdir}_{name}" if subdir else name
    env = firmware_env()
    if env is None:
        secure_path = prebuilt_only(prebuilt / f"{name}_s.elf")
        nonsecure_path = prebuilt_only(prebuilt / f"{name}.elf")
        _declared[area, f"{declared}_s"] = (secure_path, None)
        _declared[area, declared] = (nonsecure_path, None)
        return secure_path, nonsecure_path, None

    secure_path, secure, nonsecure_path, nonsecure = trustzone_pair(
        f"firmware-{area}-{declared}",
        name,
        env,
        env,
        secure_sources,
        nonsecure_sources,
        secure_ld,
        nonsecure_ld,
        prebuilt,
    )
    for target in (secure, nonsecure):
        target.private.include_dirs.extend(include_dirs)
        target.private.defines.extend(defines)
    _declared[area, f"{declared}_s"] = (secure_path, secure)
    _declared[area, declared] = (nonsecure_path, nonsecure)
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
