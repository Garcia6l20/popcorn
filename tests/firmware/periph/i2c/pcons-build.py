from pathlib import Path

from popcorn_build.stm32_firmware import STM32H573, add_ll_firmware

here = Path(__file__).resolve().parent

add_ll_firmware(
    "i2c_ll",
    [here / "i2c_ll.c", here / "i2c_target.c"],
    ["i2c"],
    ["idle", "eeprom_rw", "nack", "table", "reload", "dma", "errors", "read_errors", "target"],
)
add_ll_firmware(
    "i2c_ll",
    [here / "i2c_ll_h5.c", here / "i2c_target.c"],
    [],
    ["idle", "target"],
    chipset=STM32H573,
)
