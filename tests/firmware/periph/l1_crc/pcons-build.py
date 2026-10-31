from pathlib import Path

from popcorn_build.stm32_firmware import STM32L152XE, add_ll_firmware

here = Path(__file__).resolve().parent

add_ll_firmware(
    "l1_crc_ll",
    [here / "l1_crc_ll.c"],
    entries=["idle", "words", "dma_bytes"],
    chipset=STM32L152XE,
)
