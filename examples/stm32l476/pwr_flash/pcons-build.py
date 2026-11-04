from pathlib import Path

from popcorn_build.examples import add_example_firmware
from popcorn_build.stm32_firmware import STM32L476XX

here = Path(__file__).resolve().parent

add_example_firmware(
    here,
    "pwr_flash",
    [here / "firmware" / "main.c"],
    hal=["flash", "flash_ex"],
    chipset=STM32L476XX,
)
