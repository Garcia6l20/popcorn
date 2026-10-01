from pathlib import Path

from popcorn_build.examples import add_example_firmware

here = Path(__file__).resolve().parent

add_example_firmware(here, "pwr_flash", [here / "firmware" / "main.c"])
