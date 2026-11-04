from pathlib import Path

from popcorn_build.examples import add_example_firmware
from popcorn_build.stm32_firmware import STM32L476XX

here = Path(__file__).resolve().parent
firmware = here / "firmware"

add_example_firmware(
    here,
    "spi_accel",
    [firmware / "main.c", firmware / "stm32l4xx_hal_msp.c"],
    hal=["spi"],
    chipset=STM32L476XX,
)
