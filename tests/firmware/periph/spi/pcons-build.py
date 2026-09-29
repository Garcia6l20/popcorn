from pathlib import Path

from popcorn_build.stm32_firmware import add_ll_firmware

here = Path(__file__).resolve().parent

add_ll_firmware(
    "spi_ll",
    [here / "spi_ll.c"],
    ["spi", "gpio"],
    ["idle", "poll", "irq_eot", "no_cs", "hw_ss", "write_read", "erase", "dma"],
)
