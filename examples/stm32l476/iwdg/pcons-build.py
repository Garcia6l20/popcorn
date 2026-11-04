from pathlib import Path

from popcorn_build.examples import add_example_firmware
from popcorn_build.stm32_firmware import STM32L476XX

here = Path(__file__).resolve().parent

add_example_firmware(
    here, "iwdg_refresh", [here / "firmware" / "main.c"], hal=["iwdg"], chipset=STM32L476XX
)
