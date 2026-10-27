from pathlib import Path

from popcorn_build.stm32_firmware import STM32H573, add_ll_firmware

here = Path(__file__).resolve().parent

add_ll_firmware("pwr_ll", [here / "pwr_ll.c"])
add_ll_firmware("pwr_ll", [here / "pwr_ll_h5.c"], chipset=STM32H573)
