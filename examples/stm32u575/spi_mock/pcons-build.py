from pathlib import Path

from popcorn_build.examples import add_example_firmware

here = Path(__file__).resolve().parent
firmware = here / "firmware"

add_example_firmware(
    here,
    "spi_adc",
    [firmware / "main.c", firmware / "stm32u5xx_it.c", firmware / "stm32u5xx_hal_msp.c"],
    hal=["spi", "spi_ex", "dma", "dma_ex"],
)
