import os
import subprocess
import sys
from dataclasses import replace
from pathlib import Path
from types import SimpleNamespace

import pytest

from popcorn_build import examples, firmware, stm32_firmware
from popcorn_build.config import build_dir
from popcorn_build.stm32_firmware import (
    STM32H573,
    STM32L152XB,
    STM32L152XE,
    STM32U575,
    Chipset,
    add_ll_firmware,
    add_trustzone_cmsis_firmware,
)

PREBUILT = Path(__file__).parents[2] / "firmware" / "periph" / "prebuilt"
CHIPSETS = [STM32U575, STM32H573]


@pytest.mark.parametrize("chipset", CHIPSETS, ids=lambda chipset: chipset.name)
def test_a_chipset_file_names_follow_its_define_and_family(chipset: Chipset) -> None:
    stem = chipset.define.lower()
    assert chipset.name == stem.removesuffix("xx")
    assert chipset.startup == f"startup_{stem}.s"
    assert chipset.system == f"system_stm32{chipset.family}xx.c"
    assert chipset.nonsecure_system == f"system_stm32{chipset.family}xx_ns.c"
    assert chipset.linker == f"{chipset.define}_FLASH.ld"
    assert chipset.secure_linker == f"{chipset.define}_FLASH_s.ld"
    assert chipset.nonsecure_linker == f"{chipset.define}_FLASH_ns.ld"
    assert chipset.hal_package == f"stm32{chipset.family}-hal-driver"
    assert chipset.device_package == f"cmsis-device-{chipset.family}"
    assert chipset.defines == (chipset.define, "USE_FULL_LL_DRIVER")


@pytest.mark.parametrize("chipset", CHIPSETS, ids=lambda chipset: chipset.name)
def test_a_chipset_test_linker_script_and_prebuilt_boot_firmware_exist(chipset: Chipset) -> None:
    assert (Path(__file__).parents[3] / chipset.test_linker).is_file()
    assert (PREBUILT / chipset.prebuilt_dir / "boot_ll.elf").is_file()


def test_the_h573_prebuilt_firmware_lives_in_its_own_subdirectory() -> None:
    assert STM32U575.prebuilt_dir == ""
    assert STM32H573.prebuilt_dir == "h573"
    assert STM32H573.usb_device_sources == (
        "portable/st/stm32_fsdev/dcd_stm32_fsdev.c",
        "portable/st/stm32_fsdev/fsdev_common.c",
    )


@pytest.mark.parametrize("chipset", CHIPSETS, ids=lambda chipset: chipset.name)
def test_without_the_cross_toolchain_the_committed_prebuilt_elf_is_used(
    chipset: Chipset, monkeypatch: pytest.MonkeyPatch
) -> None:
    monkeypatch.setattr(firmware, "toolchain_found", False)
    monkeypatch.setattr(firmware, "_envs", {})
    monkeypatch.setattr(firmware, "_declared", {})
    elf, target = add_ll_firmware("boot_ll", [], ["rcc"], chipset=chipset)
    declared = f"{chipset.prebuilt_dir}_boot_ll" if chipset.prebuilt_dir else "boot_ll"
    assert target is None
    assert elf == PREBUILT / chipset.prebuilt_dir / "boot_ll.elf"
    assert firmware.firmware("periph", declared) == (elf, None)


L1_LIKE = Chipset(
    name="stm32l152",
    define="STM32L152xE",
    family="l1",
    hal_package="stm32l1-hal-driver",
    device_package="cmsis-device-l1",
    startup="startup_stm32l152xe.s",
    system="system_stm32l1xx.c",
    linker="STM32L152XE_FLASH.ld",
    test_linker="tests/firmware/common/m33.ld",
    usb_device_sources=(),
    prebuilt_dir="l152",
    preset=firmware.cortex_m3,
)

BUILD_PRESETS = r"""
from pathlib import Path

from pcons import Project

from popcorn_build.firmware import cortex_m3, cortex_m33, cortex_m4f, firmware_env

here = Path(__file__).resolve().parent
project = Project("presets", root_dir=here, build_dir=here / "build")
for preset in (cortex_m33, cortex_m3, cortex_m4f):
    env = firmware_env(preset)
    prefix = str(env.build_prefix)
    target = project.Program(prefix, env, sources=[here / "a.c"])
    target.private.link_flags.append(f"-Wl,-Map={here / prefix}.map")
    target.private.link_libs.append("gcc")
"""


@pytest.mark.parametrize("chipset", CHIPSETS, ids=lambda chipset: chipset.name)
def test_the_existing_chipsets_build_for_the_cortex_m33(chipset: Chipset) -> None:
    assert chipset.preset is firmware.cortex_m33
    assert chipset.require_trustzone() == (
        chipset.secure_linker,
        chipset.nonsecure_linker,
        chipset.nonsecure_system,
    )


def test_a_chipset_without_trustzone_files_is_refused_by_the_trustzone_entry_points() -> None:
    assert L1_LIKE.secure_linker is None
    with pytest.raises(SystemExit, match="stm32l152 has no TrustZone firmware files"):
        L1_LIKE.require_trustzone()
    with pytest.raises(SystemExit, match="stm32l152 has no TrustZone firmware files"):
        add_trustzone_cmsis_firmware("tz", [], [], Path("s.ld"), Path("ns.ld"), L1_LIKE)


def test_the_presets_name_their_cpu_and_have_one_build_prefix_each() -> None:
    assert firmware.cortex_m3.arch == "armv7-m"
    assert firmware.cortex_m4f.arch == "armv7e-m"
    assert "-mcpu=cortex-m3" in firmware.cortex_m3.extra_compile_flags
    assert "-mfloat-abi=soft" in firmware.cortex_m3.extra_link_flags
    assert "-mfpu=fpv4-sp-d16" in firmware.cortex_m4f.extra_compile_flags
    assert "-mfloat-abi=hard" in firmware.cortex_m4f.extra_link_flags
    assert {name: prefix for name, (_, prefix) in firmware.FIRMWARE_PRESETS.items()} == {
        "cortex-m33-none-eabi": "firmware",
        "cortex-m3-none-eabi": "firmware-m3",
        "cortex-m4f-none-eabi": "firmware-m4f",
    }


def test_a_preset_without_a_build_prefix_is_refused() -> None:
    with pytest.raises(SystemExit, match="no firmware build prefix for the preset other"):
        firmware.firmware_env(replace(firmware.cortex_m3, name="other"))


def test_a_preset_with_a_known_name_and_other_flags_is_refused() -> None:
    changed = replace(
        firmware.cortex_m3, extra_compile_flags=(*firmware.cortex_m3.extra_compile_flags, "-DX")
    )
    with pytest.raises(SystemExit, match="the registered preset cortex-m3-none-eabi, flags differ"):
        firmware.firmware_env(changed)


def test_the_trustzone_entry_tests_the_environment_it_builds_in(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    calls: list[tuple[object, ...]] = []

    def env_of(*args: object) -> None:
        calls.append(args)

    def prebuilt(*args: object, **kwargs: object) -> tuple[Path, None]:
        return Path("x.elf"), None

    monkeypatch.setattr(stm32_firmware, "firmware_env", env_of)
    monkeypatch.setattr(stm32_firmware, "add_trustzone_firmware", prebuilt)
    chipset = replace(CHIPSETS[0], preset=firmware.cortex_m4f)
    add_trustzone_cmsis_firmware("tz", [], [], Path("s.ld"), Path("ns.ld"), chipset)
    assert calls == [()]


@pytest.mark.parametrize("toolchain", [False, True])
def test_a_chipset_builds_its_firmware_with_its_own_preset(
    toolchain: bool, monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    presets: list[object] = []
    target = SimpleNamespace(
        private=SimpleNamespace(include_dirs=[], defines=[], link_flags=[], link_libs=[])
    )

    def record(*args: object, preset: object, **kwargs: object) -> tuple[Path, object]:
        presets.append(preset)
        return tmp_path / "x.elf", target

    envs: list[object] = []

    def env_of(preset: object) -> object | None:
        envs.append(preset)
        return object() if toolchain else None

    def source_of(name: str) -> Path:
        return tmp_path

    monkeypatch.setattr(stm32_firmware, "add_firmware", record)
    monkeypatch.setattr(stm32_firmware, "firmware_env", env_of)
    monkeypatch.setattr(stm32_firmware, "firmware_source", source_of)
    add_ll_firmware("boot_ll", [], chipset=L1_LIKE)
    assert presets == [firmware.cortex_m3]
    assert envs == [firmware.cortex_m3]


@pytest.mark.timeout(60)
@pytest.mark.skipif(not firmware.toolchain_found, reason="no arm-none-eabi-gcc")
def test_each_preset_links_its_own_libgcc_in_its_own_build_prefix(tmp_path: Path) -> None:
    (tmp_path / "pcons-build.py").write_text(BUILD_PRESETS)
    (tmp_path / "a.c").write_text("void _start(void) {}\nint main(void) { return 0; }\n")
    repo = Path(__file__).resolve().parents[3]
    env = {**os.environ, "PYTHONPATH": str(repo)}
    env.pop("PCONS_VARS", None)
    result = subprocess.run(
        [sys.executable, "-m", "pcons", "-B", "build"],
        cwd=tmp_path,
        env=env,
        capture_output=True,
        text=True,
        timeout=50,
        check=False,
    )
    assert result.returncode == 0, result.stdout + result.stderr
    expected = {
        "firmware": "thumb/v8-m.main+fp/hard/libgcc.a",
        "firmware-m3": "thumb/v7-m/nofp/libgcc.a",
        "firmware-m4f": "thumb/v7e-m+fp/hard/libgcc.a",
    }
    for prefix, libgcc in expected.items():
        lines = (tmp_path / f"{prefix}.map").read_text().splitlines()
        loaded = [line for line in lines if "libgcc.a" in line]
        assert loaded
        assert all(line.endswith(libgcc) for line in loaded), (prefix, loaded)
        assert (tmp_path / "build" / prefix).is_dir()


L1_CHIPSETS = [STM32L152XE, STM32L152XB]
COMMON = Path(__file__).parents[3] / "tests" / "firmware" / "common"


@pytest.mark.parametrize("chipset", L1_CHIPSETS, ids=lambda chipset: chipset.name)
def test_an_l1_chipset_builds_on_the_cortex_m3_with_the_l1_packages(chipset: Chipset) -> None:
    stem = chipset.define.lower()
    assert chipset.name == stem
    assert chipset.family == "l1"
    assert chipset.preset is firmware.cortex_m3
    assert chipset.startup == f"startup_{stem}.s"
    assert chipset.system == "system_stm32l1xx.c"
    assert chipset.hal_package == "stm32l1-hal-driver"
    assert chipset.device_package == "cmsis-device-l1"
    assert chipset.linker is None
    assert chipset.test_linker == f"tests/firmware/common/{stem}.ld"
    assert chipset.prebuilt_dir == stem.removeprefix("stm32")
    assert chipset.usb_device_sources == STM32H573.usb_device_sources
    assert chipset.defines == (chipset.define, "USE_FULL_LL_DRIVER")
    with pytest.raises(SystemExit, match=f"{chipset.name} has no TrustZone firmware files"):
        chipset.require_trustzone()


@pytest.mark.parametrize("chipset", L1_CHIPSETS, ids=lambda chipset: chipset.name)
def test_an_l1_chipset_has_no_st_linker_script_to_derive_one_from(chipset: Chipset) -> None:
    with pytest.raises(SystemExit, match=f"{chipset.name} has no ST linker script"):
        examples.st_linker_script(chipset, Path("device"), "x", 0)


@pytest.mark.parametrize("chipset", L1_CHIPSETS, ids=lambda chipset: chipset.name)
def test_the_l1_prebuilt_smoke_firmware_lives_in_the_chipset_directory(chipset: Chipset) -> None:
    assert (PREBUILT / chipset.prebuilt_dir / "l1_smoke.elf").is_file()


@pytest.mark.parametrize(
    ("chipset", "flash", "sram"),
    [(STM32L152XE, "256K", "32K"), (STM32L152XB, "128K", "16K")],
    ids=lambda value: getattr(value, "name", value),
)
def test_an_l1_linker_script_is_sized_for_the_smallest_part_it_runs_on(
    chipset: Chipset, flash: str, sram: str
) -> None:
    text = (Path(__file__).parents[3] / chipset.test_linker).read_text()
    assert f"FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = {flash}" in text
    assert f"SRAM  (rwx) : ORIGIN = 0x20000000, LENGTH = {sram}" in text
    assert "_estack = ORIGIN(SRAM) + LENGTH(SRAM);" in text


@pytest.mark.parametrize("chipset", L1_CHIPSETS, ids=lambda chipset: chipset.name)
def test_an_l1_linker_script_copies_ram_functions_with_the_data(chipset: Chipset) -> None:
    data = next(
        line
        for line in (Path(__file__).parents[3] / chipset.test_linker).read_text().splitlines()
        if line.strip().startswith(".data")
    )
    assert "*(.RamFunc) *(.RamFunc*)" in data
    assert data.endswith("> SRAM AT > FLASH")


@pytest.mark.parametrize("chipset", L1_CHIPSETS, ids=lambda chipset: chipset.name)
@pytest.mark.skipif(not firmware.toolchain_found, reason="no arm-none-eabi-gcc")
def test_the_l1_smoke_firmware_links_the_cortex_m3_libgcc(chipset: Chipset) -> None:
    map_file = (
        build_dir / "firmware-m3" / "tests" / "firmware" / "periph" / "l1_smoke"
        / f"{chipset.name.removeprefix('stm32')}_l1_smoke.map"
    )  # fmt: skip
    if not map_file.is_file():
        pytest.skip("the smoke firmware is not built")
    loaded = [line for line in map_file.read_text().splitlines() if "libgcc.a" in line]
    assert loaded
    assert all(line.endswith("thumb/v7-m/nofp/libgcc.a") for line in loaded), loaded
