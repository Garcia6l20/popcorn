from pathlib import Path

from popcorn_build.stm32_firmware import add_ll_firmware

here = Path(__file__).resolve().parent

add_ll_firmware(
    "irq_ll",
    [here / "irq_ll.c"],
    ["rcc", "tim", "usart"],
    [
        "irq_rate",
        "irq_wfi",
        "irq_unmask",
        "spin_store",
        "uart_tx",
        "irq_period_cycles",
        "irq_stores",
    ],
)
