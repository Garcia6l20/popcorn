from pathlib import Path

from popcorn_build.examples import add_example_firmware

here = Path(__file__).resolve().parent

add_example_firmware(here, "iwdg_refresh", [here / "firmware" / "main.c"], hal=["iwdg"])
