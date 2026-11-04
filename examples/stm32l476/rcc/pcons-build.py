from pathlib import Path

from popcorn_build.examples import add_example_firmware
from popcorn_build.stm32_firmware import STM32L476XX

here = Path(__file__).resolve().parent

add_example_firmware(
    here, "rcc_clock", [here / "firmware" / "main.c"], hal=["rcc"], chipset=STM32L476XX
)
