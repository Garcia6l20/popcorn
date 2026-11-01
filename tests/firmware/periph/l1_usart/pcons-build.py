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
                "stm32l1xx_hal_pwr.c",
                "stm32l1xx_hal_rcc.c",
                "stm32l1xx_hal_uart.c",
            )
        ]
        if firmware_env(chipset.preset) is not None
        else []
    )
    _, target = add_ll_firmware(
        "l1_usart_ll",
        [here / "l1_usart_ll.c", *hal],
        entries=[
            "idle",
            "tx_irq",
            "rx_irq",
            "over8_poll",
            "tc_clear",
            "nine_bits",
            "uart4_poll",
            "dma_tx",
            "dma_rx",
            "idle_clear",
            "late_idle",
            "overrun",
            "hal_tx_rx",
        ],
        chipset=chipset,
    )
    if target is not None:
        target.private.include_dirs.append(here)
        target.private.compile_flags.append("-Wno-unused-parameter")
