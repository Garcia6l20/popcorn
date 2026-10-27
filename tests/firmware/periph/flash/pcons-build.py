from pathlib import Path

from popcorn_build.stm32_firmware import STM32H573, add_ll_firmware

here = Path(__file__).resolve().parent

add_ll_firmware(
    "flash_ll",
    [here / "flash_ll.c"],
    entries=[
        "wrong_key",
        "words",
        "copy_words",
        "store_half",
        "store_byte",
        "store_double",
        "wait_idle",
        "spin",
        "burst_program",
        "launch_options",
        "after_launch",
    ],
)

add_ll_firmware(
    "flash_ll",
    [here / "flash_ll_h5.c"],
    entries=[
        "words",
        "copy_words",
        "store_half",
        "store_byte",
        "wait_idle",
        "spin",
        "boot_write",
        "read_word",
    ],
    chipset=STM32H573,
)

add_ll_firmware(
    "power_cut_ll",
    [here / "power_cut_ll.c"],
    entries=["nmi_count", "ecc_address", "append", "stored", "erase_log", "erasing", "scan"],
)
