from pathlib import Path

from popcorn_build.firmware import firmware_env
from popcorn_build.requirements import firmware_source
from popcorn_build.stm32_firmware import STM32L476XX, add_ll_firmware

here = Path(__file__).resolve().parent

add_ll_firmware(
    "l4_exti_ll", [here / "l4_exti_ll.c"], ["gpio", "exti"], ["idle"], chipset=STM32L476XX
)

hal = (
    [
        firmware_source(STM32L476XX.hal_package) / "Src" / name
        for name in ("stm32l4xx_hal.c", "stm32l4xx_hal_cortex.c")
    ]
    if firmware_env(STM32L476XX.preset) is not None
    else []
)
_, target = add_ll_firmware(
    "l4_syscfg_hal", [here / "l4_syscfg_hal.c", *hal], entries=["idle"], chipset=STM32L476XX
)
if target is not None:
    target.private.include_dirs.append(here)
    target.private.compile_flags.append("-Wno-unused-parameter")
