from pathlib import Path

import pytest

from popcorn_build import firmware
from popcorn_build.stm32_firmware import STM32H573, STM32U575, Chipset, add_ll_firmware

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
    monkeypatch.setattr(firmware, "_env", None)
    monkeypatch.setattr(firmware, "_declared", {})
    elf, target = add_ll_firmware("boot_ll", [], ["rcc"], chipset=chipset)
    declared = f"{chipset.prebuilt_dir}_boot_ll" if chipset.prebuilt_dir else "boot_ll"
    assert target is None
    assert elf == PREBUILT / chipset.prebuilt_dir / "boot_ll.elf"
    assert firmware.firmware("periph", declared) == (elf, None)
