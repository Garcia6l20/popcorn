from pathlib import Path

from popcorn_build.firmware import firmware_env
from popcorn_build.requirements import firmware_source
from popcorn_build.stm32_firmware import STM32L152XB, STM32L152XE, add_ll_firmware

here = Path(__file__).resolve().parent

for chipset in (STM32L152XE, STM32L152XB):
    hal = (
        [
            firmware_source(chipset.hal_package) / "Src" / name
            for name in (
                "stm32l1xx_hal_flash_ramfunc.c",
                "stm32l1xx_hal_flash.c",
                "stm32l1xx_hal_flash_ex.c",
            )
        ]
        if firmware_env(chipset.preset) is not None
        else []
    )
    _, target = add_ll_firmware(
        "l1_flash_ll",
        [here / "l1_flash_ll.c", *hal],
        entries=[
            "idle",
            "wrong_key",
            "words",
            "copy_words",
            "store_byte",
            "store_half",
            "wait_idle",
            "store_loop",
            "half_page_hal",
            "erase_page",
            "load_word",
            "read_flash",
            "parallel_erase_hal",
            "parallel_half_hal",
            "copy_words_flash",
            "store_pair",
            "eeprom_hal",
        ],
        chipset=chipset,
    )
    if target is not None:
        target.private.include_dirs.append(here)
        target.private.compile_flags.append("-Wno-unused-parameter")
