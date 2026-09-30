from pathlib import Path

from popcorn_build.examples import add_example_firmware

here = Path(__file__).resolve().parent
firmware = here / "firmware"

add_example_firmware(
    here,
    "gpio_button",
    [firmware / "main.c", firmware / "stm32u5xx_it.c"],
    hal=["gpio"],
)
