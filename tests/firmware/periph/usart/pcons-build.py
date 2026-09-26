from pathlib import Path

from popcorn_build.stm32_firmware import add_ll_firmware

here = Path(__file__).resolve().parent

add_ll_firmware(
    "usart_ll",
    [here / "usart_ll.c"],
    ["rcc", "usart", "lpuart"],
    [
        "tx_irq",
        "rx_irq",
        "loop_fifo",
        "overrun",
        "uart4_poll",
        "lpuart_poll",
        "idle",
        "tx_fifo_irq",
        "tx_dma",
    ],
)
