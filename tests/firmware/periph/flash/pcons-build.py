from pathlib import Path

from popcorn_build.stm32_firmware import add_ll_firmware

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
    ],
)
