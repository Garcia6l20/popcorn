from pathlib import Path

from popcorn_build.stm32_firmware import add_ll_firmware

here = Path(__file__).resolve().parent

add_ll_firmware("gpio_ll", [here / "gpio_ll.c"], ["gpio"])
