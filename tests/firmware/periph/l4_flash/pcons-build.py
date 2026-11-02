from pathlib import Path

from popcorn_build.stm32_firmware import STM32L476XX, add_ll_firmware

here = Path(__file__).resolve().parent

add_ll_firmware(
    "l4_flash_ll",
    [here / "l4_flash_ll.c"],
    [],
    [
        "idle",
        "wrong_key",
        "words",
        "pattern",
        "copy_words",
        "copy_words_ram",
        "store_half",
        "store_byte",
        "wait_idle",
        "read_word",
        "nmi_eccr",
        "nmi_count",
    ],
    chipset=STM32L476XX,
)
