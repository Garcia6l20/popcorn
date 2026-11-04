from pathlib import Path

from popcorn_build.firmware import firmware_env
from popcorn_build.requirements import firmware_source
from popcorn_build.stm32_firmware import STM32L476XX, add_ll_firmware

here = Path(__file__).resolve().parent
shared = here.parent / "i2c"

_, ll = add_ll_firmware(
    "l4_i2c_ll",
    [here / "l4_i2c_ll.c", shared / "i2c_target.c"],
    ["i2c"],
    ["idle", "hsi16_100k", "target"],
    chipset=STM32L476XX,
)
if ll is not None:
    ll.private.include_dirs.append(shared)

hal = (
    [
        firmware_source(STM32L476XX.hal_package) / "Src" / name
        for name in (
            "stm32l4xx_hal.c",
            "stm32l4xx_hal_cortex.c",
            "stm32l4xx_hal_dma.c",
            "stm32l4xx_hal_i2c.c",
            "stm32l4xx_hal_pwr.c",
            "stm32l4xx_hal_pwr_ex.c",
            "stm32l4xx_hal_rcc.c",
        )
    ]
    if firmware_env(STM32L476XX.preset) is not None
    else []
)
_, target = add_ll_firmware(
    "l4_i2c_hal", [here / "l4_i2c_hal.c", *hal], entries=["idle"], chipset=STM32L476XX
)
if target is not None:
    target.private.include_dirs.append(here)
    target.private.compile_flags.append("-Wno-unused-parameter")
