from pathlib import Path

from popcorn_build.stm32_firmware import add_ll_firmware

here = Path(__file__).resolve().parent

add_ll_firmware(
    "i2c_ll",
    [here / "i2c_ll.c"],
    ["i2c"],
    ["idle", "eeprom_rw", "nack", "table", "reload"],
)
