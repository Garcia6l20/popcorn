from pathlib import Path

from popcorn_build.stm32_firmware import add_ll_firmware

here = Path(__file__).resolve().parent

add_ll_firmware(
    "hash_ll",
    [here / "hash_ll.c"],
    [],
    [
        "idle",
        "hash_poll",
        "hash_backtoback",
        "hash_irq",
        "hash_dma",
        "hmac_poll",
        "hmac_irq",
        "hmac_dma",
        "hash_swap",
        "hash_dma_swap",
        "hash_done",
        "hash_count",
        "hash_digest",
        "hash_cycles",
        "hash_ticks",
        "hash_dinis_irqs",
        "hash_dcis_irqs",
        "hash_busy_feeds",
        "hash_dma_count",
        "hash_dma_digest",
        "hash_dma_control",
        "hmac_count",
        "hmac_digest",
        "hmac_dinis_irqs",
        "hmac_dma_count",
        "hmac_dma_digest",
        "swap_count",
        "swap_digest",
        "dma_swap_count",
        "dma_swap_digest",
        "dma_swap_flags",
        "dma_swap_words",
    ],
)
