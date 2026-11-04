from pathlib import Path

from popcorn_build.firmware import firmware_env
from popcorn_build.requirements import firmware_source
from popcorn_build.stm32_firmware import STM32L476XX, add_ll_firmware

here = Path(__file__).resolve().parent

hal = (
    [
        firmware_source(STM32L476XX.hal_package) / "Src" / name
        for name in (
            "stm32l4xx_hal.c",
            "stm32l4xx_hal_cortex.c",
            "stm32l4xx_hal_dma.c",
            "stm32l4xx_hal_spi.c",
            "stm32l4xx_hal_spi_ex.c",
        )
    ]
    if firmware_env(STM32L476XX.preset) is not None
    else []
)
_, target = add_ll_firmware(
    "l4_spi_ll",
    [here / "l4_spi_ll.c", *hal],
    entries=[
        "idle",
        "master8",
        "ds16",
        "ds4",
        "packed16",
        "odd_packed",
        "crc8",
        "crc16_ds8",
        "overrun",
        "rx_only",
        "irq",
        "hal_txrx",
        "hal_txrx_dma_odd",
        "hal_crc",
    ],
    chipset=STM32L476XX,
)
if target is not None:
    target.private.include_dirs.append(here)
    target.private.compile_flags.append("-Wno-unused-parameter")
