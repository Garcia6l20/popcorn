from pathlib import Path

from popcorn_build.examples import add_example_firmware

here = Path(__file__).resolve().parent
firmware = here / "firmware"

add_example_firmware(
    here,
    "spi_flash",
    [
        firmware / "app_header.c",
        firmware / "main.c",
        firmware / "spi_exchange.c",
        firmware / "stm32u5xx_it.c",
        firmware / "stm32u5xx_hal_msp.c",
        firmware / "stm32u5xx_hal_timebase_tim.c",
    ],
    hal=["spi", "tim", "tim_ex", "dma", "dma_ex"],
    freertos=True,
    flash_offset=0xC000,
)
