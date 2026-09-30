from pathlib import Path

from popcorn_build.examples import add_example_firmware

here = Path(__file__).resolve().parent

add_example_firmware(
    here,
    "i2c_eeprom",
    [here / "firmware" / "main.c"],
    ll=["i2c", "gpio"],
)
