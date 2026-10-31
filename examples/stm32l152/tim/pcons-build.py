from pathlib import Path

from popcorn_build.examples import add_example_firmware
from popcorn_build.stm32_firmware import STM32L152XE

here = Path(__file__).resolve().parent

add_example_firmware(
    here, "tim_ticks", [here / "firmware" / "main.c"], hal=["tim", "tim_ex"], chipset=STM32L152XE
)
