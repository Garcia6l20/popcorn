from pathlib import Path

from popcorn_build.stm32_firmware import STM32L152XB, STM32L152XE, add_ll_firmware

here = Path(__file__).resolve().parent

for chipset in (STM32L152XE, STM32L152XB):
    add_ll_firmware(
        "l1_spi_ll",
        [here / "l1_spi_ll.c"],
        [],
        ["idle", "master8", "master16", "crc8", "crc16", "dma_crc", "overrun", "rx_only", "irq"],
        chipset=chipset,
    )
