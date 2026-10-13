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
        "hash_done",
        "hash_count",
        "hash_digest",
        "hash_cycles",
        "hash_ticks",
        "hash_dinis_irqs",
        "hash_dcis_irqs",
        "hash_busy_feeds",
    ],
)
