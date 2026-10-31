from pathlib import Path

from popcorn_build.examples import add_example_firmware
from popcorn_build.stm32_firmware import STM32L152XB, STM32L152XE

here = Path(__file__).resolve().parent
firmware = here / "firmware"

for chipset, name in ((STM32L152XE, "usart_echo"), (STM32L152XB, "usart_echo_xb")):
    add_example_firmware(
        here,
        name,
        [firmware / "main.c", firmware / "stm32l1xx_it.c", firmware / "stm32l1xx_hal_msp.c"],
        hal=["uart", "dma"],
        chipset=chipset,
    )
