from pathlib import Path

from popcorn_build.examples import add_example_firmware
from popcorn_build.stm32_firmware import STM32L476XX

here = Path(__file__).resolve().parent

add_example_firmware(
    here,
    "dac_triangle",
    [here / "firmware" / "main.c"],
    hal=["adc", "adc_ex", "dac", "dac_ex", "tim", "tim_ex"],
    chipset=STM32L476XX,
)
