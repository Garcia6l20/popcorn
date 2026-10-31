from pathlib import Path

from popcorn_build.examples import add_example_firmware
from popcorn_build.stm32_firmware import STM32L152XB, STM32L152XE

here = Path(__file__).resolve().parent

for chipset, name in ((STM32L152XE, "pwr_flash"), (STM32L152XB, "pwr_flash_xb")):
    add_example_firmware(
        here, name, [here / "firmware" / "main.c"], hal=["flash", "flash_ex"], chipset=chipset
    )
