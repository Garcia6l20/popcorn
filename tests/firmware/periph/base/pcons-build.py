from pathlib import Path

from popcorn_build.stm32_firmware import STM32H573, add_ll_firmware

here = Path(__file__).resolve().parent

add_ll_firmware("boot_ll", [here / "boot_ll.c"], ["rcc"])
add_ll_firmware("boot_ll", [here / "boot_ll.c"], ["rcc"], chipset=STM32H573)
