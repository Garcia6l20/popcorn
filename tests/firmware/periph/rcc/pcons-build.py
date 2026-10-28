from pathlib import Path

from popcorn_build.stm32_firmware import STM32H573, add_ll_firmware

here = Path(__file__).resolve().parent

add_ll_firmware("rcc_ll", [here / "rcc_ll.c"], ["rcc"], ["idle"])
add_ll_firmware("rcc_ll", [here / "rcc_ll_h5.c"], ["rcc"], ["idle"], chipset=STM32H573)
