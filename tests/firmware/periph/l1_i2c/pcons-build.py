from pathlib import Path

from popcorn_build.firmware import firmware_env
from popcorn_build.requirements import firmware_source
from popcorn_build.stm32_firmware import STM32L152XB, STM32L152XE, add_ll_firmware

here = Path(__file__).resolve().parent

for chipset in (STM32L152XE, STM32L152XB):
    hal = (
        [
            firmware_source(chipset.hal_package) / "Src" / name
            for name in (
                "stm32l1xx_hal.c",
                "stm32l1xx_hal_cortex.c",
                "stm32l1xx_hal_dma.c",
                "stm32l1xx_hal_gpio.c",
                "stm32l1xx_hal_i2c.c",
                "stm32l1xx_hal_pwr.c",
                "stm32l1xx_hal_rcc.c",
            )
        ]
        if firmware_env(chipset.preset) is not None
        else []
    )
    _, target = add_ll_firmware(
        "l1_i2c_ll",
        [here / "l1_i2c_ll.c", *hal],
        [],
        [
            "idle",
            "write_eeprom",
            "nack",
            "read_one",
            "read_two",
            "read_four",
            "hal_read_one",
            "hal_read_two",
            "hal_read_three",
            "hal_read_none",
            "hal_ready",
        ],
        chipset=chipset,
    )
    if target is not None:
        target.private.include_dirs.append(here)
        target.private.compile_flags.append("-Wno-unused-parameter")
