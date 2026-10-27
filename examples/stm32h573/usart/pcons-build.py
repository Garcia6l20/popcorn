from pathlib import Path

from popcorn_build.examples import add_example_firmware
from popcorn_build.stm32_firmware import STM32H573

here = Path(__file__).resolve().parent
firmware = here / "firmware"

add_example_firmware(
    here,
    "usart_echo",
    [
        firmware / "main.c",
        firmware / "stm32h5xx_it.c",
        firmware / "stm32h5xx_hal_msp.c",
    ],
    hal=["uart", "uart_ex", "dma", "dma_ex"],
    chipset=STM32H573,
)
