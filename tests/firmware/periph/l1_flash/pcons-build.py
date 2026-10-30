from pathlib import Path

from popcorn_build.stm32_firmware import STM32L152XB, STM32L152XE, add_ll_firmware

here = Path(__file__).resolve().parent

for chipset in (STM32L152XE, STM32L152XB):
    add_ll_firmware(
        "l1_flash_ll",
        [here / "l1_flash_ll.c"],
        entries=[
            "idle",
            "wrong_key",
            "words",
            "copy_words",
            "store_byte",
            "store_half",
            "wait_idle",
            "store_loop",
        ],
        chipset=chipset,
    )
