from pathlib import Path

from popcorn_build.examples import add_example_firmware

here = Path(__file__).resolve().parent

add_example_firmware(
    here,
    "rcc_clock",
    [here / "firmware" / "main.c"],
    defines=["HSE_VALUE=32000000U"],
)
