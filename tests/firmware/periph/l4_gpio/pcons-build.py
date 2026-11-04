from pathlib import Path

from popcorn_build.stm32_firmware import STM32L476XX, add_ll_firmware

here = Path(__file__).resolve().parent

add_ll_firmware("l4_gpio_ll", [here / "l4_gpio_ll.c"], ["gpio"], ["idle"], chipset=STM32L476XX)
