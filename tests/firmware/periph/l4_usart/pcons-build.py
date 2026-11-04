from pathlib import Path

from popcorn_build.firmware import firmware_env
from popcorn_build.requirements import firmware_source
from popcorn_build.stm32_firmware import STM32L476XX, add_ll_firmware

here = Path(__file__).resolve().parent

add_ll_firmware(
    "l4_usart_ll",
    [here / "l4_usart_ll.c"],
    entries=[
        "idle",
        "hsi16_poll",
        "uart4_tx_irq",
        "uart4_rx_irq",
        "lpuart_lse",
        "lpuart_pclk1",
        "dma_tx",
        "dma_rx",
        "idle_overrun",
    ],
    chipset=STM32L476XX,
)

hal = (
    [
        firmware_source(STM32L476XX.hal_package) / "Src" / name
        for name in (
            "stm32l4xx_hal.c",
            "stm32l4xx_hal_cortex.c",
            "stm32l4xx_hal_dma.c",
            "stm32l4xx_hal_pwr.c",
            "stm32l4xx_hal_pwr_ex.c",
            "stm32l4xx_hal_rcc.c",
            "stm32l4xx_hal_uart.c",
            "stm32l4xx_hal_uart_ex.c",
        )
    ]
    if firmware_env(STM32L476XX.preset) is not None
    else []
)
_, target = add_ll_firmware(
    "l4_usart_hal", [here / "l4_usart_hal.c", *hal], entries=["idle"], chipset=STM32L476XX
)
if target is not None:
    target.private.include_dirs.append(here)
    target.private.compile_flags.append("-Wno-unused-parameter")
