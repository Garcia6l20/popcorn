from pathlib import Path

from popcorn_build.stm32_firmware import STM32L476XX, add_ll_firmware

here = Path(__file__).resolve().parent

add_ll_firmware(
    "l4_usart_ll",
    [here / "l4_usart_ll.c"],
    entries=["idle", "hsi16_poll", "uart4_tx_irq", "uart4_rx_irq"],
    chipset=STM32L476XX,
)
