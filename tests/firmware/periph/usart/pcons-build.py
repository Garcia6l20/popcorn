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
        "rx_dma_list",
        "brr_while_enabled",
        "link_tx",
        "link_rx",
        "link_abort",
        "link_rx_slow",
        "link_rx_2stop",
        "link_rx_at",
        "link_tx_pattern",
        "link_tx_switch",
        "link_tx_55",
        "link_rx_over8",
        "link_rx_onebit",
    ],
)
